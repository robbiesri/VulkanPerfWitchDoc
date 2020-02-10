#include "GPUVoyeur.h"

void GPUVoyeur::LogWriterThreadLoop()
{
    // Work can build up across multiple threads at sub-microsecond granularity
    // This seems like a decent first ballpark for sleep time, though I speculate
    // micro-second times are fine as long as we have enough room in the queue buffers
    const auto emptyQueueSleep = std::chrono::microseconds(50);

    while (!m_isMPSCQueueConstructed);

    m_isLogWriterThreadActive = true;

    while (m_isLogWriterThreadActive)
    {
        std::this_thread::sleep_for(emptyQueueSleep);
    }
}