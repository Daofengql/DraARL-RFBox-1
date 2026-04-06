SET(SOURCES ui_StartUP.c
    ui_main.c
    ui.c
    ui_comp_hook.c
    ui_helpers.c
    ui_img_wifi_bad_png.c
    ui_img_bluetooth_disabled_24dp_e3e3e3_fill0_wght400_grad0_opsz24_png.c
    ui_img_browser_updated_24dp_e3e3e3_fill0_wght400_grad0_opsz24_png.c
    ui_img_settings_24dp_e3e3e3_fill0_wght400_grad0_opsz24_png.c
    ui_img_cloud_sync_24dp_e3e3e3_fill0_wght400_grad0_opsz24_png.c
    ui_img_cloud_alert_24dp_e3e3e3_fill0_wght400_grad0_opsz24_png.c
    ui_img_bluetooth_24dp_e3e3e3_fill0_wght400_grad0_opsz24_png.c
    ui_img_bluetooth_connected_24dp_e3e3e3_fill0_wght400_grad0_opsz24_png.c
    ui_img_cloud_done_24dp_e3e3e3_fill0_wght400_grad0_opsz24_png.c
    ui_img_wifi_1_png.c
    ui_img_wifi_2_png.c
    ui_img_wifi_3_png.c
    ui_img_wifi_4_png.c
    ui_img_wifi_off_png.c)

add_library(ui ${SOURCES})
