#include "PCH.h"
#include "PapyrusOptimizer.h"
#include "RuntimePolicy.h"

namespace VRLoadingScreens
{
    void PapyrusOptimizer::Init(float maxFPS, float budgetBase)
    {
        if (m_externalOwner.load(std::memory_order_acquire)) {
            m_enabled.store(false, std::memory_order_release);
            logger::info(
                "DynamicUpdateBudget: disabled because HFPF owns "
                "fUpdateBudgetMS:Papyrus");
            return;
        }

        float fpsMin = 60.0f;
        float fpsMax = std::clamp(maxFPS, fpsMin, 300.0f);
        float base = std::clamp(budgetBase, 0.1f, 4.0f);

        if (fpsMax <= fpsMin) {
            logger::warn("DynamicUpdateBudget: maxFPS ({}) <= 60, disabling", fpsMax);
            return;
        }

        // Compute multiplier: scales frame interval to budget milliseconds
        // At 60fps (1/60s interval), budget = budgetBase ms
        m_bmult = base / (1.0f / 60.0f * 1000.0f) * 1000.0f;
        m_t_max = 1.0f / fpsMin;   // longest acceptable interval (60fps)
        m_t_min = 1.0f / fpsMax;   // shortest acceptable interval (maxFPS)
        m_lastInterval = m_t_max;

        // Cache the game setting pointer
        m_budgetSetting = RE::GetINISetting("fUpdateBudgetMS:Papyrus");
        if (!m_budgetSetting) {
            logger::warn("DynamicUpdateBudget: could not find fUpdateBudgetMS:Papyrus");
            return;
        }

        m_enabled.store(true, std::memory_order_release);
        logger::info("DynamicUpdateBudget: base={} ms, range=[{:.4f}, {:.4f}], bmult={:.2f}",
            base, m_t_min * m_bmult, m_t_max * m_bmult, m_bmult);
    }

    void PapyrusOptimizer::Update()
    {
        if (!Policy::ShouldRunPapyrusUpdater(
                m_enabled.load(std::memory_order_acquire),
                m_gameSessionActive.load(std::memory_order_acquire),
                m_loading.load(std::memory_order_acquire),
                m_externalOwner.load(std::memory_order_acquire))) {
            return;
        }

        // Read current frame delta from BSTimer
        auto* timer = RE::BSTimer::GetSingleton();
        if (!timer) return;

        float interval = std::clamp(timer->delta, m_t_min, m_t_max);

        // Asymmetric smoothing: decrease instantly, increase slowly
        // This prevents Papyrus budget from spiking on a single slow frame
        if (interval <= m_lastInterval) {
            m_lastInterval = interval;
        } else {
            m_lastInterval = std::min(m_lastInterval + interval * 0.0075f, interval);
        }

        float budget = m_lastInterval * m_bmult;
        m_pendingBudget.store(budget, std::memory_order_relaxed);

        // Update() runs from the VR Submit callback. RE::Setting is game-owned
        // state, so write it on the F4SE game-task queue rather than from the
        // render thread. Four updates per second are plenty for this smoothed
        // budget and avoid flooding the queue at 90 Hz.
        const auto nowTicks = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto lastTicks = m_lastQueueTicks.load(std::memory_order_relaxed);
        const auto elapsed = std::chrono::steady_clock::duration(nowTicks - lastTicks);
        if (elapsed < std::chrono::milliseconds(250)) return;
        m_lastQueueTicks.store(nowTicks, std::memory_order_relaxed);
        if (m_applyTaskQueued.exchange(true, std::memory_order_acq_rel)) return;

        if (auto* tasks = F4SE::GetTaskInterface()) {
            tasks->AddTask([this]() {
                if (Policy::ShouldRunPapyrusUpdater(
                        m_enabled.load(std::memory_order_acquire),
                        m_gameSessionActive.load(std::memory_order_acquire),
                        m_loading.load(std::memory_order_acquire),
                        m_externalOwner.load(std::memory_order_acquire)) &&
                    m_budgetSetting) {
                    m_budgetSetting->SetFloat(m_pendingBudget.load(std::memory_order_relaxed));
                }
                m_applyTaskQueued.store(false, std::memory_order_release);
            });
        } else {
            m_applyTaskQueued.store(false, std::memory_order_release);
        }
    }

    void PapyrusOptimizer::SetGameSessionActive(bool a_active) noexcept
    {
        m_gameSessionActive.store(a_active, std::memory_order_release);
    }

    void PapyrusOptimizer::SetLoading(bool a_loading) noexcept
    {
        m_loading.store(a_loading, std::memory_order_release);
    }

    void PapyrusOptimizer::SetExternalOwner(bool a_owned) noexcept
    {
        m_externalOwner.store(a_owned, std::memory_order_release);
        if (a_owned) {
            m_enabled.store(false, std::memory_order_release);
        }
    }
}
