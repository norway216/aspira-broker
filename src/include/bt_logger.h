#ifndef BT_LOGGER_H
#define BT_LOGGER_H

#ifdef __cplusplus
extern "C" {
#endif

/* ── Ring-Buffer Logger ────────────────────────────────────────────── */

void bt_log_init(void);
void bt_log_set_level(int level);
void bt_log_set_file(const char *path);
void bt_log(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void bt_log_flush(void);

#define BT_LOG_ERR(...)   bt_log(0, __VA_ARGS__)
#define BT_LOG_WARN(...)  bt_log(1, __VA_ARGS__)
#define BT_LOG_INFO(...)  bt_log(2, __VA_ARGS__)
#define BT_LOG_DEBUG(...) bt_log(3, __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* BT_LOGGER_H */
