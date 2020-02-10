/*
* Copyright (c) 2018 Google Corporation
*
* Author: Robert Srinivasiah <robsri@google.com>
*/

#pragma once

#include <vulkan/vulkan.h>

#include "layerSettings.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <list>
#include <memory>
#include <mutex>
#include <queue>
#include <stack>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "concurrentqueue.h"

#include "commandTracker.h"
#include "gvUtils.h"
#include "binaryLog.h"
#include "socketDefs.h"

struct LayerBypassDispatch {
    // instance functions, used for layer-managed query pool setup
    PFN_vkGetPhysicalDeviceProperties getPhysicalDeviceProperties;
    PFN_vkGetPhysicalDeviceFeatures getPhysicalDeviceFeatures;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties getPhysicalDeviceQueueFamilyProperties;

    // device functions, used for layer-managed query pool setup
    PFN_vkAllocateCommandBuffers allocateCommandBuffers;
    PFN_vkBeginCommandBuffer beginCommandBuffer;
    PFN_vkCmdResetQueryPool cmdResetQueryPool; // also usage
    PFN_vkCreateCommandPool createCommandPool;
    PFN_vkCreateFence createFence;
    PFN_vkCreateQueryPool createQueryPool;
    PFN_vkDestroyCommandPool destroyCommandPool;
    PFN_vkDestroyFence destroyFence;
    PFN_vkEndCommandBuffer endCommandBuffer;
    PFN_vkFreeCommandBuffers freeCommandBuffers;
    PFN_vkGetDeviceQueue getDeviceQueue;
    PFN_vkGetFenceStatus getFenceStatus;
    PFN_vkQueueSubmit queueSubmit;

    // device functions, used for layer-managed query pool usage
    PFN_vkCmdWriteTimestamp cmdWriteTimestamp;
    PFN_vkCmdBeginQuery cmdBeginQuery;
    PFN_vkCmdEndQuery cmdEndQuery;
    PFN_vkGetQueryPoolResults getQueryPoolResults;
};

// Thoughts on tracking submits and queries
// What should my data structure be? I want to do two things.
// 1. I want to iterate over the submits in submitted order to check status
// 2. I want to figure out which submit preceded a present, so when the submit
//    is processed
// My initial implementation is to create a set of queues per VkQueue, where
// entries get dropped in by submits, and then processed somewhere else (present
// time or worker thread). The tracked submits will contain the CBs, slots, and
// possible associated frame presents.

// TODO: Fence monitoring (deferred)
// Initially, I thought I could monitor each Fence, but it gets tricky
// with application interactions (what to do at Reset if fence is not signaled?)
// Maybe we can try it later
// Additionally, there should be a VkFence. If not, we can create one.
// We can use the fence to track the submission status. However, the app can
// manipulate the fence so we need to be able to check fences at any time
//  vkDestroyFence - we can defer or just spin
//  vkResetFences - we can spin...not sure how we defer the reset...
// As for checking fences, we can intercept:
//  vkWaitForFences
//  vkGetFenceStatus
// Plus we need our own time to check them (at VkQueuePresent time for now)
// TODO: CB monitoring (deferred)
// I could also check the CB state itself
// TODO: Inject semaphore (deferred)
// I could also try injecting a semaphore into VkSubmitInfo.pSignalSemaphores 

struct MarkerState
{
    std::string markerText;
    uint32_t slotIndex = UINT32_MAX;
};

struct PerCmdBufMarkers
{
    std::vector<MarkerState> markerList;
    std::stack<uint32_t> markerIndexStack;
};

using PerCmdBufMarkersPtr = std::unique_ptr<PerCmdBufMarkers>;

struct TrackedSubmitInfo {
    VkQueue m_queue = VK_NULL_HANDLE;
    bool m_presentOnlySubmit = false;
    uint64_t m_usSinceDeviceCreate;

    std::vector<uint32_t> m_cbSetSlotIndices;
    std::vector<MarkerState> m_markers;
    std::vector<uint32_t> m_resetSlotIndices;
};
typedef std::list<TrackedSubmitInfo> QueueSubmitList;

class GPUVoyeur {

public:
    GPUVoyeur();
    ~GPUVoyeur();

    // API injection points

    VkResult PostCallCreateInstance(const VkInstanceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkInstance* pInstance);
    VkResult PreCallCreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo * pCreateInfo, const VkAllocationCallbacks * pAllocator, VkDevice * pDevice);
    VkResult PostCallCreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo * pCreateInfo, const VkAllocationCallbacks * pAllocator, VkDevice * pDevice);

    // command buffer management tracking for timestamp injection
    VkResult PostCallCreateCommandPool(VkDevice device, const VkCommandPoolCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkCommandPool* pCommandPool);
    void PostCallDestroyCommandPool(VkDevice device, VkCommandPool commandPool, const VkAllocationCallbacks* pAllocator);
    VkResult PostCallResetCommandPool(VkDevice device, VkCommandPool commandPool, VkCommandPoolResetFlags flags);
    VkResult PostCallAllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo* pAllocateInfo, VkCommandBuffer* pCommandBuffers);
    void PostCallFreeCommandBuffers(VkDevice device, VkCommandPool commandPool, uint32_t commandBufferCount, const VkCommandBuffer* pCommandBuffers);

    // timing management
    VkResult PostCallBeginCommandBuffer(VkCommandBuffer commandBuffer, const VkCommandBufferBeginInfo* pBeginInfo);
    VkResult PreCallEndCommandBuffer(VkCommandBuffer commandBuffer);
    VkResult PreCallResetCommandBuffer(VkCommandBuffer commandBuffer, VkCommandBufferResetFlags flags);
    void PostCallCmdDebugMarkerBeginEXT(VkCommandBuffer commandBuffer, const VkDebugMarkerMarkerInfoEXT* pMarkerInfo);
    void PreCallCmdDebugMarkerEndEXT(VkCommandBuffer commandBuffer);
    
    // queue monitoring and readback
    void PostCallGetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, VkQueue* pQueue);
    VkResult PreCallQueueSubmit(VkQueue queue, uint32_t submitCount, const VkSubmitInfo* pSubmits, VkFence fence);
    VkResult PostCallQueueSubmit(VkQueue queue, uint32_t submitCount, const VkSubmitInfo* pSubmits, VkFence fence);
    VkResult PostCallQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo);
    VkResult PreCallAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t* pImageIndex);

    // TODO: Does this _have_ to be public?
    void LogWriterThreadLoop();

private:

    // general state
    bool m_layerInfrastructureSetup;
    uint64_t m_presentCount;
    std::ofstream m_outFileStream;
    std::ofstream m_debugLogStream;
    std::chrono::time_point<std::chrono::high_resolution_clock> m_deviceCreateTime;

    enum CaptureMode{
        Mixed = 0,
        Local,
        Network,
    };

    // settings
    bool m_loggerThreadRequested;
    std::string m_outputPath;
    uint32_t m_port;
    CaptureMode m_captureMode;
    LayerSettings m_fileSettings;

    // worker thread state state
    moodycamel::ConcurrentQueue<uint32_t> m_mpscQueue;
    std::thread m_logWriterThread;
    volatile bool m_isLogWriterThreadActive;
    volatile bool m_isMPSCQueueConstructed;

    LayerBypassDispatch m_layerBypassDispatch;

    // extracted instance/device state
    VkInstance    m_instance;
    VkDevice    m_device;
    float m_timestampPeriod;

    VkQueryPool m_timerQueryPool;

    // TODO: This pool has to be managed differently
    // It needs half the slots the timestamp pool does. It's
    // not tied to the same places that timestamps can be used.
    VkQueryPool m_pipelineStatsQueryPool;
    bool m_pipelineStatsEnabled;

    // tracked information for query pool reset
    // TODO: Use concurrentqueue to transmit reset slots from
    // submit-processing producer to BeginCommandBuffer consumer
    CommandPoolAndBufferTracker m_resetCommandTracker;
    std::atomic<bool> m_resetNeeded;
    std::vector<uint32_t> m_pendingResetSlots;
    std::mutex m_pendingResetSlotMutex;

    // timestamp management
    std::mutex m_cbToSlotMutex;
    std::unordered_map<VkCommandBuffer, uint32_t> m_cmdBufLogicalSlotMap;
    CommandPoolAndBufferTracker m_timestampCommandTracker;

    std::mutex m_markerLock;
    volatile uint32_t m_maxMarkerDepth;
    std::unordered_map<VkCommandBuffer, PerCmdBufMarkersPtr> m_cmdBufMarkerInfo;
    // TODO: Investigate whether we can build a system that guarantees element
    // access if unrelated elements are created/deleted (I don't think maps guarantee this).
    // Unique pointers guarantee access once the pointer is obtained. But we can't guarantee
    // the read/fetch will succeed during map add/delete.
    // TODO: Something else to consider is if we can run into scenarios where the pointer
    // is obtained for PerCmdBufMarkers, but then it is deleted from the map (because of a reset
    // or delete). Should we protect against that? Maybe we want to make it a shared pointer access?
    // TODO: Should we have a pool of these PerCmdBufMarkers instead of constant alloc/re-alloc?

    // TODO: Use this to synchronize file access until 
    // we can offload work to the worker thread
    std::mutex m_logMutex;

    QueueFamilyInfoManager m_qfInfoManager;

    std::mutex m_globalIndexMutex;
    std::unordered_map<VkQueue, uint8_t> m_queueHandleToGlobalIndexMap;
    std::vector<QueueInfoPacket> m_queueInfoCache;

    // TODO: Make this concurrentqueue with VkQueue as producer token
    // TODO: Do I dare think about just submitting all tracked submits into
    // a shared queue? Is it possible to maintain the queue ordering in order
    // to figure out the preceding timestamp? Probably not...
    std::mutex m_queueSubmitLock;
    std::unordered_map<VkQueue, QueueSubmitList> m_perQueueSubmitTracker;

    QuerySlotManager m_querySlotManager;

    // Fill in at reset time, pop off at submit time, attach to submit info
    std::mutex m_cbToResetSlotMutex;
    std::unordered_map<VkCommandBuffer, std::vector<uint32_t>> m_commandBufferToSlotResetsMap;

    // HUD frametime estimate ONLY
    std::mutex m_frametimeEstimateMutex;
    std::unordered_map<VkQueue, uint32_t> m_finalSlotInQueueTracker;
    std::queue<uint32_t> m_estimatedSlotForPresentQueue;

    GvSocket m_listenerSocket;
    GvSocket m_liveConnectionSocket;
    GvSocket m_captureSocket;
    uint32_t m_numFramesToCapture;

    static const uint32_t kStagingBufferSize = 1024;
    char m_writeStagingBuffer[kStagingBufferSize];
    uint32_t m_stagingPut;

    // helper infra routines
    void initLayerInfrastructure();
    void loadSettingsFromFile();
    void openOutputFile();

    void PopulateInstanceLayerBypassDispatchTable();
    void PopulateDeviceLayerBypassDispatchTable();
    PFN_vkVoidFunction GetDeviceProcAddr_DispatchHelper(const char *pName);
    PFN_vkVoidFunction GetInstanceProcAddr_DispatchHelper(const char *pName);

    // API utils
    void InitializeQueryPools(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo * pCreateInfo);
    void RemoveCommandPoolAssociatedMarkerState(VkCommandPool pool);

    uint32_t GenTimerPairIndexFromSlot(uint32_t slotIndex);
    bool IssueQueryRangeStart(VkCommandBuffer commandBuffer, VkPipelineStageFlagBits pipelineStage, uint32_t& allocatedSlotIndex);
    void IssueQueryRangeEnd(VkCommandBuffer commandBuffer, uint32_t allocatedSlotIndex);

    void ResetCommandBufferTrackedSlots(VkCommandBuffer commandBuffer);
    void ResetCommandBuffersFromCommandPool(VkCommandPool pool);

    void AddTrackedSubmits(VkQueue queue, uint32_t submitCount, const VkSubmitInfo* pSubmits);
    void MarkRecentSubmitForPresent(VkQueue queue, uint32_t presentIndex);
    void ProcessCompletedSubmits();
    void PruneCompletedFrametimeSlots();

    // manage data being written out
    void WriteData(uint32_t writeSize, const char* writeData);
    void FlushStagingBuffer();
    void FlushToOutput(uint32_t writeSize, const char* writeData);

    // sockets stuff
    void CreateListenerSocketForClient();
    void SetSocketNonBlockingState(GvSocket socket, int nonblockingState);
    void CloseGvSocket(GvSocket& socket);
    void ShutDownSocketInfra();

    void CheckForConnectionFromClient();
    void SendFrametimeToClient(uint64_t timestamp);
    
    void CheckForCaptureRequest();
    void TransmitCaptureData(uint32_t writeSize, const char* writeData);
    void DecrementCaptureFrameCount();
};