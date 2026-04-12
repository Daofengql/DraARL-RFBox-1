#ifndef NET_AUDIO_LINK_H
#define NET_AUDIO_LINK_H

bool net_audio_link_init();
void net_audio_link_on_main_screen_enter();
void net_audio_link_update();
void net_audio_link_schedule_radio_config_sync();
void net_audio_link_hide_bind_popup();
bool net_audio_link_is_bind_popup_visible();

#endif // NET_AUDIO_LINK_H
