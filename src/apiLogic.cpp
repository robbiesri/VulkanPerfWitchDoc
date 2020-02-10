#include "GPUVoyeur.h"
#include "layerCore.h"
#include "binaryLog.h"

#include <sstream>

VkResult GPUVoyeur::PostCallCreateInstance(const VkInstanceCreateInfo * pCreateInfo, const VkAllocationCallbacks * pAllocator, VkInstance * pInstance)
{
    initLayerInfrastructure();

    m_instance = *pInstance;

    PopulateInstanceLayerBypassDispatchTable();

    return VK_SUCCESS;
}

VkResult GPUVoyeur::PreCallCreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo * pCreateInfo, const VkAllocationCallbacks * pAllocator, VkDevice * pDevice)
{
    VkPhysicalDeviceFeatures physDevFeatures = {};
    m_layerBypassDispatch.getPhysicalDeviceFeatures(physicalDevice, &physDevFeatures);

    if (VK_TRUE == physDevFeatures.pipelineStatisticsQuery)
    {
        // sometimes the app doesn't request pipeline stats
        const_cast<VkPhysicalDeviceFeatures*>(pCreateInfo->pEnabledFeatures)->pipelineStatisticsQuery = VK_TRUE;
        m_pipelineStatsEnabled = true;
    }
    else
    {
        m_pipelineStatsEnabled = false;
    }

    // TODO: Not all stats will be enabled for each queue family
    // Can we figure out which stats will be actually valid per queue family?

    return VK_SUCCESS;
}

VkResult GPUVoyeur::PostCallCreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo * pCreateInfo, const VkAllocationCallbacks * pAllocator, VkDevice * pDevice)
{
    m_device = *pDevice;

    m_timerQueryPool = VK_NULL_HANDLE;
    m_pipelineStatsQueryPool = VK_NULL_HANDLE;

    m_resetNeeded = false;

    // 1. create the pools here
    // 2. reset them
    // 3. submit the reset
 

    PopulateDeviceLayerBypassDispatchTable();
    InitializeQueryPools(physicalDevice, pCreateInfo);

    m_deviceCreateTime = std::chrono::high_resolution_clock::now();

    // after our pools are created and reset, the QuerySlotManager can assume all slots/pairs
    // are in the ReadyForQueryIssue state

    LogHeader logHeader;
    logHeader.handshake = kLogHandshake;
    logHeader.version = kCurrentVersion;
    logHeader.timestampPeriod = m_timestampPeriod;
    
    const char* logHeaderPtr = reinterpret_cast<const char *>(&logHeader);
    WriteData(sizeof(LogHeader), logHeaderPtr);

    CreateListenerSocketForClient();

    // TODO: Determine whether timestamps work on transfer queues or not
    // Timestamps are supported on any queue which reports a non-zero value for timestampValidBits via vkGetPhysicalDeviceQueueFamilyProperties.
    // If the timestampComputeAndGraphics limit is VK_TRUE, timestamps are supported by every queue family that supports either graphics or compute operations
    // It's not clear to me that timestamps are supported at ALL on transfer queues?

    return VK_SUCCESS;
}

PFN_vkVoidFunction GPUVoyeur::GetDeviceProcAddr_DispatchHelper(const char *pName)
{
    return GV::GvGetDispatchedDeviceProcAddr(m_device, pName);
}

PFN_vkVoidFunction GPUVoyeur::GetInstanceProcAddr_DispatchHelper(const char *pName)
{
    return GV::GvGetDispatchedInstanceProcAddr(m_instance, pName);
}

void GPUVoyeur::PopulateInstanceLayerBypassDispatchTable()
{
    m_layerBypassDispatch.getPhysicalDeviceProperties = (PFN_vkGetPhysicalDeviceProperties)GetInstanceProcAddr_DispatchHelper("vkGetPhysicalDeviceProperties");
    m_layerBypassDispatch.getPhysicalDeviceFeatures = (PFN_vkGetPhysicalDeviceFeatures)GetInstanceProcAddr_DispatchHelper("vkGetPhysicalDeviceFeatures");
    m_layerBypassDispatch.getPhysicalDeviceQueueFamilyProperties = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)GetInstanceProcAddr_DispatchHelper("vkGetPhysicalDeviceQueueFamilyProperties");
}

void GPUVoyeur::PopulateDeviceLayerBypassDispatchTable()
{
    m_layerBypassDispatch.allocateCommandBuffers = (PFN_vkAllocateCommandBuffers)GetDeviceProcAddr_DispatchHelper("vkAllocateCommandBuffers");
    m_layerBypassDispatch.beginCommandBuffer = (PFN_vkBeginCommandBuffer)GetDeviceProcAddr_DispatchHelper("vkBeginCommandBuffer");
    m_layerBypassDispatch.cmdResetQueryPool = (PFN_vkCmdResetQueryPool)GetDeviceProcAddr_DispatchHelper("vkCmdResetQueryPool");
    m_layerBypassDispatch.createCommandPool = (PFN_vkCreateCommandPool)GetDeviceProcAddr_DispatchHelper("vkCreateCommandPool");
    m_layerBypassDispatch.createFence = (PFN_vkCreateFence)GetDeviceProcAddr_DispatchHelper("vkCreateFence");
    m_layerBypassDispatch.createQueryPool = (PFN_vkCreateQueryPool)GetDeviceProcAddr_DispatchHelper("vkCreateQueryPool");
    m_layerBypassDispatch.destroyCommandPool = (PFN_vkDestroyCommandPool)GetDeviceProcAddr_DispatchHelper("vkDestroyCommandPool");
    m_layerBypassDispatch.destroyFence = (PFN_vkDestroyFence)GetDeviceProcAddr_DispatchHelper("vkDestroyFence");
    m_layerBypassDispatch.endCommandBuffer = (PFN_vkEndCommandBuffer)GetDeviceProcAddr_DispatchHelper("vkEndCommandBuffer");
    m_layerBypassDispatch.freeCommandBuffers = (PFN_vkFreeCommandBuffers)GetDeviceProcAddr_DispatchHelper("vkFreeCommandBuffers");
    m_layerBypassDispatch.getDeviceQueue = (PFN_vkGetDeviceQueue)GetDeviceProcAddr_DispatchHelper("vkGetDeviceQueue");
    m_layerBypassDispatch.getFenceStatus = (PFN_vkGetFenceStatus)GetDeviceProcAddr_DispatchHelper("vkGetFenceStatus");
    m_layerBypassDispatch.queueSubmit = (PFN_vkQueueSubmit)GetDeviceProcAddr_DispatchHelper("vkQueueSubmit");

    m_layerBypassDispatch.cmdWriteTimestamp = (PFN_vkCmdWriteTimestamp)GetDeviceProcAddr_DispatchHelper("vkCmdWriteTimestamp");
    m_layerBypassDispatch.cmdBeginQuery = (PFN_vkCmdBeginQuery)GetDeviceProcAddr_DispatchHelper("vkCmdBeginQuery");
    m_layerBypassDispatch.cmdEndQuery = (PFN_vkCmdEndQuery)GetDeviceProcAddr_DispatchHelper("vkCmdEndQuery");
    m_layerBypassDispatch.getQueryPoolResults = (PFN_vkGetQueryPoolResults)GetDeviceProcAddr_DispatchHelper("vkGetQueryPoolResults");
}

void GPUVoyeur::InitializeQueryPools(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo * pCreateInfo)
{
    VkQueryPoolCreateInfo timerCreateInfo = {};
    timerCreateInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    timerCreateInfo.pNext = nullptr;
    timerCreateInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
    timerCreateInfo.queryCount = kNumPhysicalTimerQuerySlots;

    VkResult res = m_layerBypassDispatch.createQueryPool(m_device, &timerCreateInfo, nullptr, &m_timerQueryPool);
    assert(res == VK_SUCCESS);

    if (m_pipelineStatsEnabled)
    {
        VkQueryPoolCreateInfo pipelineStatsCreateInfo = {};
        pipelineStatsCreateInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        pipelineStatsCreateInfo.pNext = nullptr;
        pipelineStatsCreateInfo.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;

        // TODO: Detect when there are new statistics available?
        pipelineStatsCreateInfo.pipelineStatistics =
            VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT |
            VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT |
            VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
            VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_INVOCATIONS_BIT |
            VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_PRIMITIVES_BIT |
            VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT |
            VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT |
            VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT |
            VK_QUERY_PIPELINE_STATISTIC_TESSELLATION_CONTROL_SHADER_PATCHES_BIT |
            VK_QUERY_PIPELINE_STATISTIC_TESSELLATION_EVALUATION_SHADER_INVOCATIONS_BIT |
            VK_QUERY_PIPELINE_STATISTIC_COMPUTE_SHADER_INVOCATIONS_BIT;
        pipelineStatsCreateInfo.queryCount = kNumPhysicalStatQuerySlots;

        res = m_layerBypassDispatch.createQueryPool(m_device, &pipelineStatsCreateInfo, nullptr, &m_pipelineStatsQueryPool);
        assert(res == VK_SUCCESS);

    }

    VkPhysicalDeviceProperties physDevProps = {};
    m_layerBypassDispatch.getPhysicalDeviceProperties(physicalDevice, &physDevProps);
    m_timestampPeriod = physDevProps.limits.timestampPeriod;

    uint32_t count = 0;
    m_layerBypassDispatch.getPhysicalDeviceQueueFamilyProperties(physicalDevice, &count, nullptr);

    std::vector<VkQueueFamilyProperties> qfPropertiesList;
    if (count > 0) 
    {
        qfPropertiesList.resize(count);
        m_layerBypassDispatch.getPhysicalDeviceQueueFamilyProperties(physicalDevice, &count, qfPropertiesList.data());
    }

    m_qfInfoManager.BuildQFInfoState(qfPropertiesList);

    uint32_t resetFamilyIdx = UINT32_MAX;
    for (uint32_t testIndex = 0; testIndex < count; testIndex++)
    {
        if (m_qfInfoManager.DoesQFSupportTimestampResets(testIndex))
        {
            for (uint32_t createdIndex = 0; createdIndex < pCreateInfo->queueCreateInfoCount; createdIndex++)
            {
                if (pCreateInfo->pQueueCreateInfos->queueFamilyIndex == testIndex)
                {
                    resetFamilyIdx = testIndex;
                    break;
                }
            }
        }
    }
    assert(resetFamilyIdx != UINT32_MAX);

    VkQueue queueForReset = VK_NULL_HANDLE;
    m_layerBypassDispatch.getDeviceQueue(m_device, resetFamilyIdx, 0, &queueForReset);

    // command pool time!
    VkCommandPoolCreateInfo cpci = {};
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.pNext = nullptr;
    cpci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    cpci.queueFamilyIndex = resetFamilyIdx;
    VkCommandPool cmdPool = VK_NULL_HANDLE;

    res = m_layerBypassDispatch.createCommandPool(m_device, &cpci, nullptr, &cmdPool);
    assert(res == VK_SUCCESS);

    VkCommandBuffer resetCmdBuf = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cbai = {};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.pNext = nullptr;
    cbai.commandPool = cmdPool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;

    res = m_layerBypassDispatch.allocateCommandBuffers(m_device, &cbai, &resetCmdBuf);
    assert(res == VK_SUCCESS);

    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.pNext = nullptr;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    beginInfo.pInheritanceInfo = nullptr;

    res = m_layerBypassDispatch.beginCommandBuffer(resetCmdBuf, &beginInfo);
    assert(res == VK_SUCCESS);
            
    m_layerBypassDispatch.cmdResetQueryPool(resetCmdBuf, m_timerQueryPool, 0, kNumPhysicalTimerQuerySlots);

    if (m_pipelineStatsEnabled)
    {
        m_layerBypassDispatch.cmdResetQueryPool(resetCmdBuf, m_pipelineStatsQueryPool, 0, kNumPhysicalStatQuerySlots);
    }

    res = m_layerBypassDispatch.endCommandBuffer(resetCmdBuf);
    assert(res == VK_SUCCESS);

    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.pNext = nullptr;
    submitInfo.waitSemaphoreCount = 0;
    submitInfo.pWaitSemaphores = nullptr;
    submitInfo.pWaitDstStageMask = 0;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &resetCmdBuf;
    submitInfo.signalSemaphoreCount = 0;
    submitInfo.pSignalSemaphores = nullptr;

    VkFenceCreateInfo fci = {};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.pNext = nullptr;
    fci.flags = 0;
    VkFence resetFence = VK_NULL_HANDLE;
    
    res = m_layerBypassDispatch.createFence(m_device, &fci, nullptr, &resetFence);
    assert(res == VK_SUCCESS);

    res = m_layerBypassDispatch.queueSubmit(queueForReset, 1, &submitInfo, resetFence);
    assert(res == VK_SUCCESS);

    while (1)
    {
        VkResult fenceStatus = m_layerBypassDispatch.getFenceStatus(m_device, resetFence);
        if (fenceStatus == VK_SUCCESS)
            break;
    }

    m_layerBypassDispatch.destroyFence(m_device, resetFence, nullptr);
    m_layerBypassDispatch.freeCommandBuffers(m_device, cmdPool, 1, &resetCmdBuf);
    m_layerBypassDispatch.destroyCommandPool(m_device, cmdPool, nullptr);
}

void GPUVoyeur::PostCallGetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, VkQueue * pQueue)
{
    VkQueueFlags qfFlags = m_qfInfoManager.GetQueueFamilyQueueFlags(queueFamilyIndex);

    if (m_queueHandleToGlobalIndexMap.find(*pQueue) == m_queueHandleToGlobalIndexMap.end())
    {
        m_globalIndexMutex.lock();
            uint32_t nextGlobalIndex = uint32_t(m_queueHandleToGlobalIndexMap.size());
            m_queueHandleToGlobalIndexMap[*pQueue] = nextGlobalIndex;
        m_globalIndexMutex.unlock();

        QueueInfoPacket queueInfoPacket;
        queueInfoPacket.packetType = PacketTypes::kQueueInfo;
        queueInfoPacket.queueFamilyFlags = uint32_t(qfFlags);
        queueInfoPacket.queueIndex = queueIndex;
        queueInfoPacket.globalQueueIndex = nextGlobalIndex;
        queueInfoPacket.handle = uint64_t(*pQueue);

        const char* queueInfoPacketPtr = reinterpret_cast<const char *>(&queueInfoPacket);
        WriteData(sizeof(QueueInfoPacket), queueInfoPacketPtr);

        m_queueInfoCache.push_back(queueInfoPacket);
    }
}

VkResult GPUVoyeur::PostCallCreateCommandPool(VkDevice device, const VkCommandPoolCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkCommandPool* pCommandPool)
{
    if (m_qfInfoManager.DoesQFSupportTimestampResets(pCreateInfo->queueFamilyIndex))
    {
        assert(VK_NULL_HANDLE != *pCommandPool);
        m_resetCommandTracker.AddPool(*pCommandPool);
    }

    if (m_qfInfoManager.DoesQFSupportTimestamps(pCreateInfo->queueFamilyIndex))
    {
        assert(VK_NULL_HANDLE != *pCommandPool);
        m_timestampCommandTracker.AddPool(*pCommandPool);
    }

    return VK_SUCCESS;
}

void GPUVoyeur::RemoveCommandPoolAssociatedMarkerState(VkCommandPool pool)
{
    if (m_timestampCommandTracker.IsCommandPoolTracked(pool))
    {
        auto& commandBuffers = m_timestampCommandTracker.GetPoolAssociatedCommandBuffers(pool);

        std::lock_guard<std::mutex> lock(m_markerLock);
        for (auto cb : commandBuffers)
        {
            m_cmdBufMarkerInfo.erase(cb);
        }
    }
}

void GPUVoyeur::PostCallDestroyCommandPool(VkDevice device, VkCommandPool commandPool, const VkAllocationCallbacks* pAllocator)
{
    ResetCommandBuffersFromCommandPool(commandPool);
    RemoveCommandPoolAssociatedMarkerState(commandPool);

    m_resetCommandTracker.RemovePool(commandPool);
    m_timestampCommandTracker.RemovePool(commandPool);
}

VkResult GPUVoyeur::PostCallResetCommandPool(VkDevice device, VkCommandPool commandPool, VkCommandPoolResetFlags flags)
{
    ResetCommandBuffersFromCommandPool(commandPool);

    return VK_SUCCESS;
}

VkResult GPUVoyeur::PreCallResetCommandBuffer(VkCommandBuffer commandBuffer, VkCommandBufferResetFlags flags)
{
    ResetCommandBufferTrackedSlots(commandBuffer);

    return VK_SUCCESS;
}

void GPUVoyeur::ResetCommandBuffersFromCommandPool(VkCommandPool pool)
{
    // TODO: Should I really just do this for the timestamp tracked ones? Seems fine to me...
    if (m_timestampCommandTracker.IsCommandPoolTracked(pool))
    {
        // TODO: This can't be a reference, this has to be a unique pointer in the future
        auto& commandBuffers = m_timestampCommandTracker.GetPoolAssociatedCommandBuffers(pool);

        for (auto cb : commandBuffers)
        {
            ResetCommandBufferTrackedSlots(cb);
        }
    }
}

void GPUVoyeur::ResetCommandBufferTrackedSlots(VkCommandBuffer commandBuffer)
{
    // Untrack reset slots associated with this CB

    std::vector<uint32_t> pendingResetSlotsToRollback;
    m_cbToResetSlotMutex.lock();

    if (m_commandBufferToSlotResetsMap.find(commandBuffer) != m_commandBufferToSlotResetsMap.end())
    {
        std::vector<uint32_t>& cbOwnedResetSlots = m_commandBufferToSlotResetsMap[commandBuffer];
        pendingResetSlotsToRollback.insert(pendingResetSlotsToRollback.end(), cbOwnedResetSlots.begin(), cbOwnedResetSlots.end());
        m_commandBufferToSlotResetsMap.erase(commandBuffer);
    }

    m_cbToResetSlotMutex.unlock();

    if (pendingResetSlotsToRollback.size() > 0)
    {
        std::lock_guard<std::mutex> lock(m_pendingResetSlotMutex);

        m_pendingResetSlots.insert(m_pendingResetSlots.end(), pendingResetSlotsToRollback.begin(), pendingResetSlotsToRollback.end());
        m_resetNeeded = true;
    }

    m_querySlotManager.RollBackSlots(pendingResetSlotsToRollback, QuerySlotManager::SlotState::ReadyForResetIssue);

    // Untrack query slots

    std::vector<uint32_t> pendingQuerySlotsToRollback;

    m_cbToSlotMutex.lock();

    bool cbTracked = (m_cmdBufLogicalSlotMap.find(commandBuffer) != m_cmdBufLogicalSlotMap.end());
    if (cbTracked)
    {
        uint32_t slotIndex = m_cmdBufLogicalSlotMap[commandBuffer];
        m_cmdBufLogicalSlotMap.erase(commandBuffer);

        pendingQuerySlotsToRollback.push_back(slotIndex); // bleh
    }
    m_cbToSlotMutex.unlock();

    if ((m_maxMarkerDepth > 0) && cbTracked)
    {
        PerCmdBufMarkers* cbMarkerInfo;
        {
            std::lock_guard<std::mutex> lock(m_markerLock);
            cbMarkerInfo = m_cmdBufMarkerInfo[commandBuffer].get();
        }

        for (auto& marker : cbMarkerInfo->markerList)
        {
            if (marker.slotIndex != UINT32_MAX)
            {
                pendingQuerySlotsToRollback.push_back(marker.slotIndex);
            }
        }
        cbMarkerInfo->markerList.clear();
    }

    m_querySlotManager.RollBackSlots(pendingQuerySlotsToRollback, QuerySlotManager::SlotState::ReadyForQueryIssue);
}

VkResult GPUVoyeur::PostCallAllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo* pAllocateInfo, VkCommandBuffer* pCommandBuffers)
{
    auto pool = pAllocateInfo->commandPool;
    auto commandBufferCount = pAllocateInfo->commandBufferCount;

    m_resetCommandTracker.AddCommandBuffers(pool, pCommandBuffers, commandBufferCount);
    m_timestampCommandTracker.AddCommandBuffers(pool, pCommandBuffers, commandBufferCount);

    if (m_timestampCommandTracker.IsCommandPoolTracked(pool))
    {
        std::lock_guard<std::mutex> lock(m_markerLock);
        for (uint32_t cbIdx = 0; cbIdx < commandBufferCount; cbIdx++)
        {
            auto& cb = pCommandBuffers[cbIdx];
            m_cmdBufMarkerInfo[cb] = std::make_unique<PerCmdBufMarkers>();
        }
    }

    return VK_SUCCESS;
}

void GPUVoyeur::PostCallFreeCommandBuffers(VkDevice device, VkCommandPool commandPool, uint32_t commandBufferCount, const VkCommandBuffer* pCommandBuffers)
{
    if (m_timestampCommandTracker.IsCommandPoolTracked(commandPool))
    {
        for (uint32_t cbIdx = 0; cbIdx < commandBufferCount; cbIdx++)
        {
            auto& cb = pCommandBuffers[cbIdx];

            ResetCommandBufferTrackedSlots(cb);

            {
                std::lock_guard<std::mutex> lock(m_markerLock);
                m_cmdBufMarkerInfo.erase(cb);
            }
        }
    }

    m_resetCommandTracker.RemoveCommandBuffers(commandPool, pCommandBuffers, commandBufferCount);
    m_timestampCommandTracker.RemoveCommandBuffers(commandPool, pCommandBuffers, commandBufferCount);
}

VkResult GPUVoyeur::PostCallBeginCommandBuffer(VkCommandBuffer commandBuffer, const VkCommandBufferBeginInfo * pBeginInfo)
{
    if (m_resetNeeded && m_resetCommandTracker.IsCommandBufferTracked(commandBuffer))
    {
        bool trueVal = true;
        if (m_resetNeeded.compare_exchange_strong(trueVal, false))
        {
            m_pendingResetSlotMutex.lock();
                auto localResetSlotList = std::move(m_pendingResetSlots);
                m_pendingResetSlots.clear();
            m_pendingResetSlotMutex.unlock();

            for (uint32_t resetSlotIndex : localResetSlotList)
            {
                uint32_t startSlot = GenTimerPairIndexFromSlot(resetSlotIndex);
                const uint32_t kNumTimerSlots = 2;
                m_layerBypassDispatch.cmdResetQueryPool(commandBuffer, m_timerQueryPool, startSlot, kNumTimerSlots);

                // TODO: Why don't I make the same checks for timestamps?
                if (m_pipelineStatsEnabled)
                {
                    m_layerBypassDispatch.cmdResetQueryPool(commandBuffer, m_pipelineStatsQueryPool, resetSlotIndex, 1);
                }
            }

            m_querySlotManager.MarkSlots(localResetSlotList, QuerySlotManager::SlotState::ResetPendingOnGPU);

            m_cbToResetSlotMutex.lock();
                m_commandBufferToSlotResetsMap.insert(std::make_pair(commandBuffer, std::move(localResetSlotList)));
            m_cbToResetSlotMutex.unlock();
        }
    }

    // now, for timing! We should allocate some timers out for this command buffer
    // we could allocate an index in the pool, and map the pool index to command buffer
    // at endcommandbuffer time, we can use that index to map the end time


    // TODO: use IssueQueryRangeStart
    if (m_timestampCommandTracker.IsCommandBufferTracked(commandBuffer))
    {
        uint32_t allocatedSlot = UINT32_MAX;
        bool validSlot = m_querySlotManager.GetNextReadyQuerySlot(allocatedSlot);
        if (validSlot)
        {
            uint32_t timerSlotBase = GenTimerPairIndexFromSlot(allocatedSlot);

            m_cbToSlotMutex.lock();
            m_cmdBufLogicalSlotMap[commandBuffer] = allocatedSlot;
            m_cbToSlotMutex.unlock();

            // We could use VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, but then we risk command buffers overlapping. The HUD doesn't process
            // that case very well right now. It's easier if they don't intersect (for now).
            m_layerBypassDispatch.cmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_timerQueryPool, timerSlotBase);

            if (m_pipelineStatsEnabled)
            {
                m_layerBypassDispatch.cmdBeginQuery(commandBuffer, m_pipelineStatsQueryPool, allocatedSlot, 0);
            }
        }
    }

    return VK_SUCCESS;
}

VkResult GPUVoyeur::PreCallEndCommandBuffer(VkCommandBuffer commandBuffer)
{
    if (m_timestampCommandTracker.IsCommandBufferTracked(commandBuffer))
    {
        if (m_maxMarkerDepth > 0)
        {
            PerCmdBufMarkers* cbMarkerInfo;
            {
                std::lock_guard<std::mutex> lock(m_markerLock);
                cbMarkerInfo = m_cmdBufMarkerInfo[commandBuffer].get();
            }

            // pop off remaining entries to close off all marker pairs
            // This has to happen before we issue the command buffer end queries
            while (cbMarkerInfo->markerIndexStack.size() > 0)
            {
                if (cbMarkerInfo->markerIndexStack.size() <= m_maxMarkerDepth)
                {
                    auto markerIndex = cbMarkerInfo->markerIndexStack.top();
                    uint32_t allocatedSlot = cbMarkerInfo->markerList[markerIndex].slotIndex;
                    bool validSlot = (allocatedSlot != UINT32_MAX);

                    if (validSlot)
                    {
                        IssueQueryRangeEnd(commandBuffer, allocatedSlot);
                    }
                }

                cbMarkerInfo->markerIndexStack.pop();
            }
        }

        uint32_t allocatedSlot = UINT32_MAX;

        m_cbToSlotMutex.lock();
        bool slotAvailable = (m_cmdBufLogicalSlotMap.find(commandBuffer) != m_cmdBufLogicalSlotMap.end());
        if (slotAvailable)
        {
            allocatedSlot = m_cmdBufLogicalSlotMap[commandBuffer];
        }
        m_cbToSlotMutex.unlock();


        if (slotAvailable)
        {
            uint32_t timerSlotBase = GenTimerPairIndexFromSlot(allocatedSlot);

            m_layerBypassDispatch.cmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_timerQueryPool, timerSlotBase + 1);

            if (m_pipelineStatsEnabled)
            {
                m_layerBypassDispatch.cmdEndQuery(commandBuffer, m_pipelineStatsQueryPool, allocatedSlot);
            }
        }
    }

    return VK_SUCCESS;
}

// TODO: Can I unify command buffer and marker range tracking?
// aren't they really the same thing, except command buffers don't have a
// label?

void GPUVoyeur::PostCallCmdDebugMarkerBeginEXT(VkCommandBuffer commandBuffer, const VkDebugMarkerMarkerInfoEXT * pMarkerInfo)
{
    if (m_maxMarkerDepth > 0)
    {
        PerCmdBufMarkers* cbMarkerInfo;
        {
            std::lock_guard<std::mutex> lock(m_markerLock);
            if (m_cmdBufMarkerInfo.find(commandBuffer) == m_cmdBufMarkerInfo.end())
            {
                return;
            }
            cbMarkerInfo = m_cmdBufMarkerInfo[commandBuffer].get();
        }

        if (cbMarkerInfo->markerIndexStack.size() < m_maxMarkerDepth)
        {
            MarkerState markerState;
            markerState.markerText = pMarkerInfo->pMarkerName;

            markerState.slotIndex = UINT32_MAX;
            if (m_timestampCommandTracker.IsCommandBufferTracked(commandBuffer))
            {
                uint32_t allocatedSlot;
                // Use bottom instead of top to prevent accidental overlap
                if (IssueQueryRangeStart(commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, allocatedSlot))
                {
                    markerState.slotIndex = allocatedSlot;
                }
            }

            uint32_t index = uint32_t(cbMarkerInfo->markerList.size());
            cbMarkerInfo->markerList.push_back(std::move(markerState));
            cbMarkerInfo->markerIndexStack.push(index);
        }
        else
        {
            // we still have to track the marker stack depth, otherwise, we'll have unmatched DebugMarkerEnd calls
            cbMarkerInfo->markerIndexStack.push(UINT32_MAX);
        }
    }
}

void GPUVoyeur::PreCallCmdDebugMarkerEndEXT(VkCommandBuffer commandBuffer)
{
    if (m_maxMarkerDepth > 0)
    {
        PerCmdBufMarkers* cbMarkerInfo;
        {
            std::lock_guard<std::mutex> lock(m_markerLock);
            if (m_cmdBufMarkerInfo.find(commandBuffer) == m_cmdBufMarkerInfo.end())
            {
                return;
            }
            cbMarkerInfo = m_cmdBufMarkerInfo[commandBuffer].get();
        }

        // grab the index from the stack, look into the vector, grab the state, do whatever
        if (cbMarkerInfo->markerIndexStack.size() > 0)
        {
            auto markerIndex = cbMarkerInfo->markerIndexStack.top();
            cbMarkerInfo->markerIndexStack.pop();

            if (cbMarkerInfo->markerIndexStack.size() < m_maxMarkerDepth)
            {
                uint32_t allocatedSlot = cbMarkerInfo->markerList[markerIndex].slotIndex;
                bool validSlot = (allocatedSlot != UINT32_MAX);

                if (validSlot)
                {
                    IssueQueryRangeEnd(commandBuffer, allocatedSlot);
                }
            }
        }
    }
}

void GPUVoyeur::PruneCompletedFrametimeSlots()
{
    m_frametimeEstimateMutex.lock();
    if (m_estimatedSlotForPresentQueue.size() > 0)
    {
        bool slotPendingOnGPU = false;
        while (!slotPendingOnGPU &&
            (m_estimatedSlotForPresentQueue.size() > 0))
        {
            uint32_t testSlot = m_estimatedSlotForPresentQueue.front();

            // TODO: SlotManager state could be changed right out under us...
            if (QuerySlotManager::SlotState::QueryPendingOnGPU == m_querySlotManager.m_slotState[testSlot])
            {
                slotPendingOnGPU = true;
            }
            else
            {
                // we already processed this slot, we can't grab the timestamp any longer :(
                m_estimatedSlotForPresentQueue.pop();
            }
        }
    }
    m_frametimeEstimateMutex.unlock();
}

// TODO: Move this somewhere else to live with the submit processing
void GPUVoyeur::ProcessCompletedSubmits()
{
    // TODO: Can there be submits without CBs? Yes? In which case, I just remove them.
    // There can't be a case of an empty submit that has a reset, since resets
    // have to be inside a real CB.

    // networking stuff
    CheckForConnectionFromClient();
    CheckForCaptureRequest();

    QueueSubmitList completedSubmitList;

    // TODO: LIMIT THE SCOPE OF THIS FUCKING LOCK
    // Make this a concurrent queue!
    m_queueSubmitLock.lock();
    for (auto& n : m_perQueueSubmitTracker)
    {
        QueueSubmitList::iterator queueSubmitIter = n.second.begin();
        while (queueSubmitIter != n.second.end())
        {
            const TrackedSubmitInfo& curSubmit = *queueSubmitIter;

            VkResult queryResult = VK_NOT_READY;
            if (curSubmit.m_cbSetSlotIndices.size() > 0)
            {
                // grab the last slot of the issued range of queries
                // Technically, this isn't absolutely correct because the last issued
                // slot doesn't mean it will be the last time. But my logic is that assuming CBs are
                // submitted in order on a queue, then the last submitted CB isn't a bad guess for
                // a sentinel
                uint32_t checkSlotIndex = curSubmit.m_cbSetSlotIndices.back();
                uint32_t finalSlotIndex = GenTimerPairIndexFromSlot(checkSlotIndex) + 1;
                VkDeviceSize result_stride = sizeof(uint64_t);
                uint64_t testQueryResult = 0;

                // TODO: Do I also want to read back the availability bit?
                queryResult = m_layerBypassDispatch.getQueryPoolResults(m_device, m_timerQueryPool,
                    finalSlotIndex, 1, sizeof(testQueryResult), &testQueryResult, result_stride, VK_QUERY_RESULT_64_BIT);
            }
            else
            {
                // if we have no tracked slots in this submit, just skip ahead
                queryResult = VK_SUCCESS;
            }

            bool submitComplete = (queryResult == VK_SUCCESS);

            if (submitComplete)
            {
                completedSubmitList.emplace_back(std::move(curSubmit));
                
                auto submitToDelete = queueSubmitIter;
                queueSubmitIter++;
                n.second.erase(submitToDelete, queueSubmitIter);
            }
            else
            {
                queueSubmitIter++;
            }
        }
        
    }

    m_queueSubmitLock.unlock();

    PruneCompletedFrametimeSlots();

    std::vector<uint32_t> readbackSlots;
    std::vector<uint32_t> resetCompleteSlots;

    // TODO: Build ranges of slots to read back so we can do bulk readbacks

    // now we can read back the data 
    for (auto& completedSubmit : completedSubmitList)
    {
        // print out queue submitted on
        m_globalIndexMutex.lock();
            uint32_t globalQueueIndex = m_queueHandleToGlobalIndexMap[completedSubmit.m_queue];
        m_globalIndexMutex.unlock();

        SubmitPacket submitPacket;
        submitPacket.packetType = PacketTypes::kSubmit;
        submitPacket.globalQueueIndex = globalQueueIndex;
        submitPacket.cpuTimeUs = completedSubmit.m_usSinceDeviceCreate;
        submitPacket.presentOnly = completedSubmit.m_presentOnlySubmit;

        submitPacket.rangeCount = uint16_t(completedSubmit.m_cbSetSlotIndices.size()); // TODO: Assert for overflow?
        submitPacket.markerCount = uint16_t(completedSubmit.m_markers.size()); // TODO: Assert for overflow?

        const char* submitPacketPtr = reinterpret_cast<const char *>(&submitPacket);
        WriteData(sizeof(SubmitPacket), submitPacketPtr);

        uint64_t finalSubmitTimestamp = UINT64_MAX;

        const uint32_t kNumTimerSlots = 2;
        
        RangeStatsPacket rangeStatsPacket;
        rangeStatsPacket.packetType = PacketTypes::kRangeStats;
        
        RangeTimerPacket rangeTimerPacket;
        rangeTimerPacket.packetType = PacketTypes::kRangeTimer;
        rangeTimerPacket.labelLength = 0;

        // Because we polled the availability of the last issued query in the submit,
        // we can assume the previous queries have been completed

        // read back and print timestamps
        uint32_t cbIndex = 0;
        for (uint32_t cbPairSlotIndex : completedSubmit.m_cbSetSlotIndices)
        {
            // TODO: Read the entire block at once...
            // TODO: Once we start reading back data in blocks, we'll want to write directly
            // into m_writeStagingBuffer. We can just turn WriteData into a two-stage method
            // to save those copies.

            // TODO: basically repeat this logic for the debug marker ranges!
            // Since this is per slot, no reason we couldn't just export a second block of
            // range count + marker count?

            readbackSlots.push_back(cbPairSlotIndex);
            uint32_t timerSlot = GenTimerPairIndexFromSlot(cbPairSlotIndex);

            VkDeviceSize          result_stride = sizeof(uint64_t);
            VkResult queryResult = m_layerBypassDispatch.getQueryPoolResults(m_device, m_timerQueryPool, 
                                                                             timerSlot, kNumTimerSlots,
                                                                             sizeof(rangeTimerPacket.timestamps), &rangeTimerPacket.timestamps[0], 
                                                                             result_stride, VK_QUERY_RESULT_64_BIT);

            const char* rangeTimerPtr = reinterpret_cast<const char *>(&rangeTimerPacket);
            WriteData(sizeof(RangeTimerPacket), rangeTimerPtr);



            if (m_pipelineStatsEnabled)
            {
                VkResult statsResult = m_layerBypassDispatch.getQueryPoolResults(m_device, m_pipelineStatsQueryPool, 
                                                                                 cbPairSlotIndex, 1, 
                                                                                 sizeof(rangeStatsPacket.stats), &rangeStatsPacket.stats[0],
                                                                                 result_stride, VK_QUERY_RESULT_64_BIT);

                const char* rangeStatsPtr = reinterpret_cast<const char *>(&rangeStatsPacket);
                WriteData(sizeof(RangeStatsPacket), rangeStatsPtr);
            }

            cbIndex++;
            finalSubmitTimestamp = rangeTimerPacket.timestamps[1];
        }

        // TODO: Merge some of this action with the processing above
        for (auto& marker : completedSubmit.m_markers)
        {
            rangeTimerPacket.labelLength = uint8_t(std::min<uint32_t>(UINT8_MAX, uint32_t(marker.markerText.size())));

            uint32_t markerSlot = marker.slotIndex;

            readbackSlots.push_back(markerSlot);
            uint32_t timerSlot = GenTimerPairIndexFromSlot(markerSlot);

            VkDeviceSize          result_stride = sizeof(uint64_t);
            VkResult queryResult = m_layerBypassDispatch.getQueryPoolResults(m_device, m_timerQueryPool,
                                                                            timerSlot, kNumTimerSlots,
                                                                            sizeof(rangeTimerPacket.timestamps), &rangeTimerPacket.timestamps[0],
                                                                            result_stride, VK_QUERY_RESULT_64_BIT);

            const char* rangeTimerPtr = reinterpret_cast<const char *>(&rangeTimerPacket);
            WriteData(sizeof(RangeTimerPacket), rangeTimerPtr);

            uint32_t labelSize = uint32_t(sizeof(char) * rangeTimerPacket.labelLength);
            WriteData(labelSize, marker.markerText.c_str());

            if (m_pipelineStatsEnabled)
            {
                VkResult statsResult = m_layerBypassDispatch.getQueryPoolResults(m_device, m_pipelineStatsQueryPool,
                                                                                 markerSlot, 1,
                                                                                 sizeof(rangeStatsPacket.stats), &rangeStatsPacket.stats[0],
                                                                                 result_stride, VK_QUERY_RESULT_64_BIT);

                const char* rangeStatsPtr = reinterpret_cast<const char *>(&rangeStatsPacket);
                WriteData(sizeof(RangeStatsPacket), rangeStatsPtr);
            }
        }

        if (completedSubmit.m_cbSetSlotIndices.size() > 0)
        {
            const uint32_t lastSlotInSubmit = completedSubmit.m_cbSetSlotIndices.back();

            m_frametimeEstimateMutex.lock();
            if ((m_estimatedSlotForPresentQueue.size() > 0) &&
                (lastSlotInSubmit == m_estimatedSlotForPresentQueue.front()))
            {
                m_estimatedSlotForPresentQueue.pop();

                // TODO: Handle bad frame times?

                if (finalSubmitTimestamp != UINT64_MAX)
                {
                    SendFrametimeToClient(finalSubmitTimestamp);
                }
                DecrementCaptureFrameCount();
            }

            m_frametimeEstimateMutex.unlock();
        }

        if (completedSubmit.m_resetSlotIndices.size() > 0)
        {
            resetCompleteSlots.insert(resetCompleteSlots.end(), completedSubmit.m_resetSlotIndices.begin(), completedSubmit.m_resetSlotIndices.end());
        }
    }

    // put the completed, readback slots into a collection that will be reset later
    if (readbackSlots.size() > 0)
    {
        // temporary hack until I merge the readback and ready for reset states
        m_querySlotManager.MarkSlots(readbackSlots, QuerySlotManager::SlotState::QueryReadbackReady);
        m_querySlotManager.MarkSlots(readbackSlots, QuerySlotManager::SlotState::ReadyForResetIssue);
        //TODO: I could make this a list of vectors to save the append

        m_pendingResetSlotMutex.lock();
            m_pendingResetSlots.insert(m_pendingResetSlots.end(), readbackSlots.begin(), readbackSlots.end());
        m_pendingResetSlotMutex.unlock();

        m_resetNeeded = true;
    }
    // now mark the slots that completed reset into the ready to issue state
    m_querySlotManager.MarkSlots(resetCompleteSlots, QuerySlotManager::SlotState::ReadyForQueryIssue);
}

VkResult GPUVoyeur::PostCallQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR * pPresentInfo)
{
    m_presentCount++;

    MarkRecentSubmitForPresent(queue, uint32_t(m_presentCount - 1));

    // TODO: This logic could be moved to the worker thread of before AcquireNextImageKHR
    ProcessCompletedSubmits();

    return VK_SUCCESS;
}

VkResult GPUVoyeur::PreCallAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t * pImageIndex)
{
    return VK_SUCCESS;
}

VkResult GPUVoyeur::PreCallQueueSubmit(VkQueue queue, uint32_t submitCount, const VkSubmitInfo* pSubmits, VkFence fence)
{
    return VK_SUCCESS;
}

VkResult GPUVoyeur::PostCallQueueSubmit(VkQueue queue, uint32_t submitCount, const VkSubmitInfo* pSubmits, VkFence fence)
{
    AddTrackedSubmits(queue, submitCount, pSubmits);

    return VK_SUCCESS;
}

