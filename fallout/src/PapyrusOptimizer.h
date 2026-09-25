#pragma once

#include <atomic>
#include <cstdint>

namespace VRLoadingScreens
{
    class PapyrusOptimizer
    {
    public:
        static PapyrusOptimizer& GetSingleton()
        {
            static PapyrusOptimizer instance;
            return instance;
        }

        void Init(float maxFPS, float budgetBase);
        void Update();
        void SetGameSessionActive(bool a_active) noexcept;
        void SetLoading(bool a_loading) noexcept;
        void SetExternalOwner(bool a_owned) noexcept;

        bool IsEnabled() const { return m_enabled.load(std::memory_order_acquire); }

    private:
        PapyrusOptimizer() = default;

        std::atomic<bool> m_enabled{ false };
        std::atomic<bool> m_gameSessionActive{ false };
        std::atomic<bool> m_loading{ false };
        std::atomic<bool> m_externalOwner{ false };
        std::atomic<bool> m_applyTaskQueued{ false };
        std::atomic<float> m_pendingBudget{ 0.0f };
        std::atomic<std::int64_t> m_lastQueueTicks{ 0 };
        float m_lastInterval = 1.0f / 60.0f;
        float m_bmult = 0.0f;
        float m_t_min = 0.0f;   // 1/maxFPS
        float m_t_max = 0.0f;   // 1/60
        RE::Setting* m_budgetSetting = nullptr;
    };
}
