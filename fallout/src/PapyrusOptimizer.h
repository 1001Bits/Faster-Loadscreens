#pragma once

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

        bool IsEnabled() const { return m_enabled; }

    private:
        PapyrusOptimizer() = default;

        bool m_enabled = false;
        float m_lastInterval = 1.0f / 60.0f;
        float m_bmult = 0.0f;
        float m_t_min = 0.0f;   // 1/maxFPS
        float m_t_max = 0.0f;   // 1/60
        RE::Setting* m_budgetSetting = nullptr;
    };
}
