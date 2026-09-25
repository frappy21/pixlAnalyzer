/**
 * Minimal nrf_log.h stub for the nrfx drivers (NFCT, TIMER). This firmware
 * has no logging backend; the drivers only need the macros to exist.
 */
#ifndef NRF_LOG_H__
#define NRF_LOG_H__

#define NRF_LOG_INFO(...)                                                          do { (void)0; } while (0)
#define NRF_LOG_DEBUG(...)                                                         do { (void)0; } while (0)
#define NRF_LOG_WARNING(...)                                                       do { (void)0; } while (0)
#define NRF_LOG_ERROR(...)                                                         do { (void)0; } while (0)
#define NRF_LOG_RAW_INFO(...)                                                      do { (void)0; } while (0)
#define NRF_LOG_RAW_DEBUG(...)                                                     do { (void)0; } while (0)
#define NRF_LOG_RAW_WARNING(...)                                                   do { (void)0; } while (0)
#define NRF_LOG_RAW_ERROR(...)                                                     do { (void)0; } while (0)
#define NRF_LOG_HEXDUMP_INFO(p_data, len)                                          do { (void)0; } while (0)
#define NRF_LOG_HEXDUMP_DEBUG(p_data, len)                                         do { (void)0; } while (0)
#define NRF_LOG_HEXDUMP_WARNING(p_data, len)                                       do { (void)0; } while (0)
#define NRF_LOG_HEXDUMP_ERROR(p_data, len)                                         do { (void)0; } while (0)
#define NRF_LOG_MODULE_REGISTER()
#define NRF_LOG_INIT(...) 0
#define NRF_LOG_PROCESS() 0

#define NRF_LOG_SEVERITY_NONE 0
#define NRF_LOG_SEVERITY_ERROR 1
#define NRF_LOG_SEVERITY_WARNING 2
#define NRF_LOG_SEVERITY_INFO 3
#define NRF_LOG_SEVERITY_DEBUG 4
#define NRF_LOG_SEVERITY_INFO_ALL 5
#define NRF_LOG_SEVERITY_DEBUG_ALL 6

#endif // NRF_LOG_H__
