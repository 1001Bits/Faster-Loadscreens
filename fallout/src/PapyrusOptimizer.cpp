#include "PCH.h"
#include "PapyrusOptimizer.h"

namespace VRLoadingScreens
{
    void PapyrusOptimizer::Init(float maxFPS, float budgetBase)
    {
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

        m_enabled = true;
        logger::info("DynamicUpdateBudget: base={} ms, range=[{:.4f}, {:.4f}], bmult={:.2f}",
            base, m_t_min * m_bmult, m_t_max * m_bmult, m_bmult);
    }

    void PapyrusOptimizer::Update()
    {
        if (!m_enabled) return;

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
        m_budgetSetting->SetFloat(budget);
    }
}
