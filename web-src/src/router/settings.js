import PageSettingsEq from '@/pages/PageSettingsEq.vue'
import PageSettingsPerformance from '@/pages/PageSettingsPerformance.vue'

// Settings pages of the speaker EQ and performance monitoring
export default [
  {
    component: PageSettingsEq,
    name: 'settings-eq',
    path: '/settings/eq'
  },
  {
    component: PageSettingsPerformance,
    name: 'settings-performance',
    path: '/settings/performance'
  }
]
