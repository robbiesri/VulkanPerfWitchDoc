#include "gvUtils.h"

#include <assert.h>

void QueueFamilyInfoManager::BuildQFInfoState(std::vector<VkQueueFamilyProperties>& qfPropertiesList)
{
    m_queueFamilyPropsList = qfPropertiesList;
    uint32_t count = static_cast<uint32_t>(m_queueFamilyPropsList.size());

    VkQueueFlags resetQueryPoolSupported = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
    for (uint32_t qfIdx = 0; qfIdx < count; qfIdx++)
    {
        if ((qfPropertiesList[qfIdx].queueFlags & resetQueryPoolSupported) != 0)
        {
            m_resetQueryPoolQFIndices.insert(qfIdx);
        }

        if (qfPropertiesList[qfIdx].timestampValidBits > 0)
        {
            m_timestampQFIndices.insert(qfIdx);
        }
    }
}

VkQueueFlags QueueFamilyInfoManager::GetQueueFamilyQueueFlags(uint32_t qfIndex)
{
    if (qfIndex > m_queueFamilyPropsList.size())
    {
        assert(!"Invalid queue family index used for flags request");
        return VkQueueFlags(0);
    }

    return m_queueFamilyPropsList[qfIndex].queueFlags;
}

bool QueueFamilyInfoManager::DoesQFSupportTimestamps(uint32_t qfIndex)
{
    return (m_timestampQFIndices.find(qfIndex) != m_timestampQFIndices.end());
}

bool QueueFamilyInfoManager::DoesQFSupportTimestampResets(uint32_t qfIndex)
{
    return (m_resetQueryPoolQFIndices.find(qfIndex) != m_resetQueryPoolQFIndices.end());
}

///////////////////////////////////////////////////
QuerySlotManager::QuerySlotManager()
{
    // this is assuming our CreateDevice interception resets our query pool
    for (uint32_t slotIndex = 0; slotIndex < kNumLogicalQuerySlots; slotIndex++)
    {
        m_slotState[slotIndex] = ReadyForQueryIssue;
    }
    m_nextFreeIndex = 0;

    m_numFreeSlots = kNumLogicalQuerySlots;
    m_numActiveSlots = 0;
}

bool QuerySlotManager::GetNextReadyQuerySlot(uint32_t& allocatedIndex)
{
    allocatedIndex = UINT32_MAX;

    m_slotMutex.lock();
    uint32_t scannerIndex = m_nextFreeIndex;
    if (ReadyForQueryIssue == m_slotState[scannerIndex])
    {
        // fast path, also 'special' case
        allocatedIndex = scannerIndex;
        m_nextFreeIndex = (m_nextFreeIndex + 1) % kNumLogicalQuerySlots;
    }
    else
    {
        scannerIndex = (m_nextFreeIndex + 1) % kNumLogicalQuerySlots;
        while ((ReadyForQueryIssue != m_slotState[scannerIndex]) &&
            (scannerIndex != m_nextFreeIndex))
        {
            scannerIndex = (scannerIndex + 1) % kNumLogicalQuerySlots;
        }

        // we successfully found a good index
        if (scannerIndex != m_nextFreeIndex)
        {
            allocatedIndex = scannerIndex;
            m_nextFreeIndex = (scannerIndex + 1) % kNumLogicalQuerySlots;
        }
    }

    if (UINT32_MAX != allocatedIndex)
    {
        m_slotState[allocatedIndex] = QueryPendingOnGPU;
        m_numFreeSlots--;
        m_numActiveSlots++;
    }

    m_slotMutex.unlock();

    return (UINT32_MAX != allocatedIndex);
}

void QuerySlotManager::MarkSlots(std::vector<uint32_t>& slotsToMark, SlotState newState)
{
    // TODO: Build a static look up table?
    // TODO: Or submit explicit previous and new states
    // to make for explicit transitions (clearer from calling code too?)

    SlotState checkState = ResetPendingOnGPU;
    if (newState > ReadyForQueryIssue)
    {
        checkState = SlotState(uint32_t(newState) - 1);
    }

    m_slotMutex.lock();
    for (uint32_t slotIndex : slotsToMark)
    {
        assert(slotIndex < kNumLogicalQuerySlots);
        if (slotIndex >= kNumLogicalQuerySlots)
            continue;

        SlotState curSlotState = m_slotState[slotIndex];
        assert(checkState == curSlotState);

        // TODO: Fail update if incorrect previous state?
        m_slotState[slotIndex] = newState;

        if (ReadyForQueryIssue == newState)
        {
            m_numFreeSlots++;
            m_numActiveSlots--;
        }
    }
    m_slotMutex.unlock();

}

void QuerySlotManager::RollBackSlots(std::vector<uint32_t>& slotsToMark, SlotState rollbackState)
{
    SlotState checkState;
    if (ReadyForQueryIssue == rollbackState)
    {
        checkState = QueryPendingOnGPU;
    }
    else if (ReadyForResetIssue == rollbackState)
    {
        checkState = ResetPendingOnGPU;
    }
    else
    {
        assert(!"Invalid query slot rollback state");
        return;
    }

    m_slotMutex.lock();
    for (uint32_t slotIndex : slotsToMark)
    {
        if (slotIndex >= kNumLogicalQuerySlots)
        {
            assert(!"Slot marking index is invalid");
            continue;
        }

        SlotState curSlotState = m_slotState[slotIndex];
        assert(checkState == curSlotState);

        // TODO: Fail update if incorrect previous state?
        m_slotState[slotIndex] = rollbackState;

        if (ReadyForQueryIssue == rollbackState)
        {
            m_numFreeSlots++;
            m_numActiveSlots--;
        }
    }
    m_slotMutex.unlock();
}

void QuerySlotManager::BuildMatchingSlotList(std::vector<uint32_t>& slots, SlotState matchState)
{
    for (uint32_t slotIndex = 0; slotIndex < kNumLogicalQuerySlots; slotIndex++)
    {
        if (matchState == m_slotState[slotIndex])
        {
            slots.push_back(slotIndex);
        }
    }
}