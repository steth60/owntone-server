import api from '@/api'

const BASE_URL = './api/outputs'

export default {
  eq(id) {
    return api.get(`${BASE_URL}/${id}/eq`)
  },
  setEq(id, profile) {
    return api.put(`${BASE_URL}/${id}/eq`, profile)
  },
  state() {
    return api.get(BASE_URL)
  },
  test(id) {
    return api.post(`${BASE_URL}/${id}/test`)
  },
  testStatus() {
    return api.get(`${BASE_URL}/test`)
  },
  toggle(id) {
    return api.put(`${BASE_URL}/${id}/toggle`)
  },
  update(id, output) {
    return api.put(`${BASE_URL}/${id}`, output)
  }
}
