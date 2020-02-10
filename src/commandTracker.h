#pragma once

#include <vulkan/vulkan.h>

#include <mutex>
#include <unordered_map>
#include <unordered_set>

typedef std::unordered_set<VkCommandBuffer> CommandBufferSet;

class CommandPoolAndBufferTracker
{
public:
    CommandPoolAndBufferTracker();
    ~CommandPoolAndBufferTracker();

    void AddPool(VkCommandPool pool);
    void RemovePool(VkCommandPool pool);
    void AddCommandBuffers(VkCommandPool pool, const VkCommandBuffer* pCommandBuffers, uint32_t count);
    void RemoveCommandBuffers(VkCommandPool pool, const VkCommandBuffer* pCommandBuffers, uint32_t count);

    bool IsCommandBufferTracked(VkCommandBuffer commandBuffer);
    void RemovePoolAssociatedCommandBuffers(VkCommandPool pool);

    bool IsCommandPoolTracked(VkCommandPool pool);
    CommandBufferSet& GetPoolAssociatedCommandBuffers(VkCommandPool pool);

private: 
    std::unordered_set<VkCommandPool> m_trackedPools;
    CommandBufferSet m_trackedCommandBuffers;
    std::unordered_map<VkCommandPool, CommandBufferSet> m_poolToBuffersMap;

    std::mutex m_mutex;
};