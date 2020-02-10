/*
 * Copyright (c) 2018 Google Corporation
 *
 * Author: Robert Srinivasiah <robsri@google.com>
 */

#include "GPUVoyeur.h"

#include <sstream>

#if defined(WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#endif


GPUVoyeur::GPUVoyeur()
{
    m_presentCount = 0;

    m_timestampPeriod = 0.f;
    m_pipelineStatsEnabled = false;

    m_outputPath.clear();
    m_isMPSCQueueConstructed = false;
    m_layerInfrastructureSetup = false;
    
    m_captureMode = CaptureMode::Mixed;

    m_stagingPut = 0;

    m_port = kLayerPortDefault;
    m_listenerSocket = (GvSocket)-1;
    m_liveConnectionSocket = (GvSocket)-1;

    m_captureSocket = (GvSocket)-1;
    m_numFramesToCapture = 0;
    m_maxMarkerDepth = 0;

    m_layerBypassDispatch = {};
}

GPUVoyeur::~GPUVoyeur()
{
    // TODO: Depending on who is writing out the buffer (worker thread?)
    // we may have to be more judicious about just flushing out the buffer
    FlushStagingBuffer();

    if (m_isLogWriterThreadActive)
    {
        m_isLogWriterThreadActive = false;
        m_logWriterThread.join();
    }

    m_outFileStream.close();
    m_debugLogStream.close();

    ShutDownSocketInfra();
}

void GPUVoyeur::initLayerInfrastructure()
{
    // Some thoughts to think:
    // * Does this code need to be locked? Would some client out there
    //	 possibly want to create two instances at the same time?

    // TODO: Any layer bits I want to consider resetting? Frame count?

    if (m_layerInfrastructureSetup)
        return;

    loadSettingsFromFile();
    openOutputFile();

    if (m_loggerThreadRequested)
    {
        // TODO: Speculate on correct size here
        m_mpscQueue = moodycamel::ConcurrentQueue<uint32_t>();
        m_isMPSCQueueConstructed = true;

        m_logWriterThread = std::thread(&GPUVoyeur::LogWriterThreadLoop, this);

        // TODO: Give threads name via platform specific APIs?
    }

    // TODO: log config status anywhere?

    m_layerInfrastructureSetup = true;
}

void GPUVoyeur::loadSettingsFromFile()
{
    m_fileSettings.openSettingsFile("GPUVoyeur");

    if (m_fileSettings.hasValidSettingsFile())
    {
        auto outputPathVal = m_fileSettings.getOption("outputPath");
        if (!outputPathVal.empty())
        {
            m_outputPath = outputPathVal;
        }

        auto loggerThreadVal = m_fileSettings.getOption("loggerThread");
        if (!loggerThreadVal.empty())
        {
            m_loggerThreadRequested = ("True" == loggerThreadVal) ||
                ("true" == loggerThreadVal);
        }

        auto portVal = m_fileSettings.getOption("port");
        if (!portVal.empty())
        {
            std::istringstream(portVal) >> m_port;
        }

        auto captureModeVal = m_fileSettings.getOption("captureMode");
        if (!captureModeVal.empty())
        {
            if (("Mixed" == captureModeVal) || ("mixed" == captureModeVal))
            {
                m_captureMode = CaptureMode::Mixed;
            }
            else if (("Local" == captureModeVal) || ("local" == captureModeVal))
            {
                m_captureMode = CaptureMode::Local;
            }
            else if (("Network" == captureModeVal) || ("network" == captureModeVal))
            {
                m_captureMode = CaptureMode::Network;
            }
        }
    }
}

void GPUVoyeur::openOutputFile()
{
    if (m_outputPath.empty())
    {

#if defined(WIN32)
        m_outputPath = getenv("USERPROFILE");
#else
        m_outputPath = getenv("HOME");
#endif

        m_outputPath += "/VkPerfHaus/";

#if defined(WIN32)
        int mkdir_result = _mkdir(m_outputPath.c_str());
#else
        auto mkdir_result = mkdir(m_outputPath.c_str(), ACCESSPERMS);
#endif

        m_outputPath += "GPUVoyeur.log";
    }
    else
    {
        struct stat info;
        if (stat(m_outputPath.c_str(), &info) == 0)
        {
            if (info.st_mode & S_IFDIR)
            {
                m_outputPath += "/GPUVoyeur.log";
            }
        }
    }

    if (CaptureMode::Network != m_captureMode)
    {
        std::ios_base::openmode outFileOpenFlags = std::ofstream::out | std::ofstream::trunc | std::ofstream::binary;
        m_outFileStream.open(m_outputPath.c_str(), outFileOpenFlags);

        if (m_outFileStream.is_open())
        {
            // TODO: Dump any default data?
        }
        else
        {
            // TODO: if we can't open the file, we gotta bail...
        }
        m_outFileStream.flush();
    }

    std::string debugLogPath = m_outputPath + ".debug";
    m_debugLogStream.open(debugLogPath.c_str(), std::ofstream::out | std::ofstream::trunc);
}

uint32_t GPUVoyeur::GenTimerPairIndexFromSlot(uint32_t slotIndex)
{
    return (slotIndex * 2);
}

bool GPUVoyeur::IssueQueryRangeStart(VkCommandBuffer commandBuffer, VkPipelineStageFlagBits pipelineStage, uint32_t & allocatedSlotIndex)
{
    allocatedSlotIndex = UINT32_MAX;
    bool validSlot = m_querySlotManager.GetNextReadyQuerySlot(allocatedSlotIndex);
    if (validSlot)
    {
        uint32_t timerSlotBase = GenTimerPairIndexFromSlot(allocatedSlotIndex);

        m_layerBypassDispatch.cmdWriteTimestamp(commandBuffer, pipelineStage, m_timerQueryPool, timerSlotBase);

        if (m_pipelineStatsEnabled)
        {
            m_layerBypassDispatch.cmdBeginQuery(commandBuffer, m_pipelineStatsQueryPool, allocatedSlotIndex, 0);
        }
    }

    return validSlot;
}

void GPUVoyeur::IssueQueryRangeEnd(VkCommandBuffer commandBuffer, uint32_t allocatedSlotIndex)
{
    uint32_t timerSlotBase = GenTimerPairIndexFromSlot(allocatedSlotIndex);

    m_layerBypassDispatch.cmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_timerQueryPool, timerSlotBase + 1);

    if (m_pipelineStatsEnabled)
    {
        m_layerBypassDispatch.cmdEndQuery(commandBuffer, m_pipelineStatsQueryPool, allocatedSlotIndex);
    }
}

/////////////////////////////////////////////////

void GPUVoyeur::AddTrackedSubmits(VkQueue queue, uint32_t submitCount, const VkSubmitInfo* pSubmits)
{
    // One of the goals here is to move data into the TrackedSubmitInfo structure, as we lose a lot
    // of assumptions after submit time. For example, if we are depending on command buffers
    // being unique (needed to map to query slots), we can't do that anymore after submit. So
    // we need to extract whatever data we need so we aren't dependent on extraneous state.

    // TODO: I want to move the TrackedSubmitInfo into a concurrentqueue. At submit time,
    // we can just insert into the queue. In ProcessCompletedSubmits, we can just peel off entries
    // into a list that's only locally accessible. 
    // There is one problem right now. Because of my multi-list setup (with one entry per queue
    // handle), once I do a process a present, I can just look at the back of the queue and tag
    // that submit as being tied to the present. Once I use concurrentqueue, I lose that
    // information. 
    // I have a couple ideas on how to figure out where the present landed.
    // 1. Keep a per-queue 'last submit' - Not sure if this works, because I don't think I can
    //    maintain a reference to something in the queue or being processed.
    // 2. Add a CPU timestamp and create present submits - We can just output
    //    the timestamp with submits, which allows us to order the submits on the HUD side
    // 3. Keep a frame counter, and just tag each submit with the current frame #
    //    This is the 'dumbest' way but might be the simplest. Ultimately, the expectation
    //    is that the app will submit the final submit and present on the same thread, in which
    //    case this should usually work.

    m_queueSubmitLock.lock();

    for (uint32_t submitIndex = 0; submitIndex < submitCount; submitIndex++)
    {
        if (m_perQueueSubmitTracker.find(queue) == m_perQueueSubmitTracker.end())
        {
            m_perQueueSubmitTracker[queue] = QueueSubmitList();
        }
        QueueSubmitList& queueSubmitList = m_perQueueSubmitTracker[queue];
        queueSubmitList.emplace_back();

        TrackedSubmitInfo& trackedSubmit = queueSubmitList.back();
        trackedSubmit.m_queue = queue;

        auto delta = std::chrono::high_resolution_clock::now() - m_deviceCreateTime;
        trackedSubmit.m_usSinceDeviceCreate = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(delta).count());

        const VkSubmitInfo& submitInfo = pSubmits[submitIndex];
        for (uint32_t cbIdx = 0; cbIdx < submitInfo.commandBufferCount; cbIdx++)
        {
            VkCommandBuffer cb = submitInfo.pCommandBuffers[cbIdx];

            // TODO: If I unify marker and CB tracking, what I should do is figure out
            // what the final 'slot' in this submit is, in order to make it easy to check later on
            // Once we unify the info, we'll lose state about the last command buffer that we know here

            m_cbToSlotMutex.lock();

            bool cbTracked = (m_cmdBufLogicalSlotMap.find(cb) != m_cmdBufLogicalSlotMap.end());
            if (cbTracked)
            {
                uint32_t slotIndex = m_cmdBufLogicalSlotMap[cb];
                m_cmdBufLogicalSlotMap.erase(cb);

                trackedSubmit.m_cbSetSlotIndices.push_back(slotIndex);
            }
            m_cbToSlotMutex.unlock();

            m_cbToResetSlotMutex.lock();
                if (m_commandBufferToSlotResetsMap.find(cb) != m_commandBufferToSlotResetsMap.end())
                {
                    std::vector<uint32_t>& trackedResetSlots = trackedSubmit.m_resetSlotIndices;
                    std::vector<uint32_t>& cbOwnedResetSlots = m_commandBufferToSlotResetsMap[cb];

                    // TODO: For now, this is a copy. This could become a move if I can convince myself of the logic
                    trackedResetSlots.insert(trackedResetSlots.end(), cbOwnedResetSlots.begin(), cbOwnedResetSlots.end());

                    m_commandBufferToSlotResetsMap.erase(cb);
                }
            m_cbToResetSlotMutex.unlock();

            if (m_maxMarkerDepth > 0)
            {
                if (cbTracked)
                {
                    PerCmdBufMarkers* cbMarkerInfo;
                    {
                        std::lock_guard<std::mutex> lock(m_markerLock);
                        cbMarkerInfo = m_cmdBufMarkerInfo[cb].get();
                    }

                    for (auto& marker : cbMarkerInfo->markerList)
                    {
                        if (marker.slotIndex != UINT32_MAX)
                        {
                            trackedSubmit.m_markers.emplace_back(std::move(marker));
                        }
                    }
                    cbMarkerInfo->markerList.clear();
                }
            }
        }

        if (trackedSubmit.m_cbSetSlotIndices.size() > 0)
        {
            uint32_t finalSlotInSubmit = trackedSubmit.m_cbSetSlotIndices.back();
            
            m_frametimeEstimateMutex.lock();
            m_finalSlotInQueueTracker[queue] = finalSlotInSubmit;
            m_frametimeEstimateMutex.unlock();
        }
    }

    m_queueSubmitLock.unlock();
}

void GPUVoyeur::MarkRecentSubmitForPresent(VkQueue queue, uint32_t presentIndex)
{
    m_queueSubmitLock.lock();
    {
        if (m_perQueueSubmitTracker.find(queue) == m_perQueueSubmitTracker.end())
        {
            m_perQueueSubmitTracker[queue] = QueueSubmitList();
        }
        QueueSubmitList& queueSubmitList = m_perQueueSubmitTracker[queue];
        queueSubmitList.emplace_back();

        TrackedSubmitInfo& trackedSubmit = queueSubmitList.back();
        trackedSubmit.m_queue = queue;

        auto delta = std::chrono::high_resolution_clock::now() - m_deviceCreateTime;
        trackedSubmit.m_usSinceDeviceCreate = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(delta).count());

        trackedSubmit.m_presentOnlySubmit = true;
    }
    m_queueSubmitLock.unlock();


    // TODO: There is the possibility that by the time this present happens,
    // the estimated slot has already been read back (chance increases with async
    // submit processing). As a work around, the queue consumer should check that the slot hasn't
    // already been processed. Maybe even store some time state ;)
    m_frametimeEstimateMutex.lock();
    if (m_finalSlotInQueueTracker.find(queue) != m_finalSlotInQueueTracker.end())
    {
        uint32_t lastSlotInQueue = m_finalSlotInQueueTracker[queue];
        m_estimatedSlotForPresentQueue.push(lastSlotInQueue);
    }
    m_frametimeEstimateMutex.unlock();
}

//////////////////////////////////////

void GPUVoyeur::WriteData(uint32_t writeSize, const char * writeData)
{
    if (writeSize > (kStagingBufferSize / 2))
    {
        FlushStagingBuffer();
        FlushToOutput(writeSize, writeData);
    }
    else
    {
        memcpy(&m_writeStagingBuffer[m_stagingPut], writeData, writeSize);
        m_stagingPut += writeSize;

        if (m_stagingPut > (kStagingBufferSize / 2))
        {
            FlushStagingBuffer();
        }
    }
}

void GPUVoyeur::FlushStagingBuffer()
{
    FlushToOutput(m_stagingPut, m_writeStagingBuffer);
    m_stagingPut = 0;
}

void GPUVoyeur::FlushToOutput(uint32_t writeSize, const char* writeData)
{
    TransmitCaptureData(writeSize, writeData);

    if (CaptureMode::Network != m_captureMode)
    {
        m_outFileStream.write(writeData, writeSize);
        m_outFileStream.flush();
    }
}