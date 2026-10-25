//
// Created by bordplate on 12/27/2022.
//

#ifndef RAC1_MULTIPLAYER_SERVERLISTVIEW_H
#define RAC1_MULTIPLAYER_SERVERLISTVIEW_H

#include <lib/vector.h>

#include "rc1/ui/View.h"
#include "rc1/ui/TextElement.h"
#include "../multiplayer/GameServer.h"

#include "rc1/ui/Input.h"
#include "rc1/ui/ListMenuElement.h"
#include "rc1/ui/TextAreaElement.h"

struct ServerListView : public View, public ListMenuDelegate {
    ServerListView();
    ~ServerListView();

    void render();
    void on_load();
    void on_unload();
    void refresh();

    void on_pressed_buttons(CONTROLLER_INPUT input);

    // ListMenuDelegate
    void on_item_activated(int index, ListMenuElement* list_menu_element);
    void on_item_selected(int index, ListMenuElement *list_menu_element);
private:
    Vector<GameServer*> servers_;
    Vector<TextElement*> servers_elements_;
    ListMenuElement* servers_list_;

    TextElement* username_label_;
    TextElement* connecting_label_;

    TextAreaElement* description_area_;
    TextAreaElement* info_area_;

    u32 frames_;

    // Set with "autoconnect=true" in settings.conf: connects to the last direct IP at boot. For automated testing.
    bool autoconnect_;

    Input username_input_;
    Input direct_ip_input_;

    void set_servers(Vector<GameServer*>* servers);

    static int username_input_callback(Input* input, void* userdata, int status);
    static int direct_ip_input_callback(Input* input, void* userdata, int status);
    static void connect_direct(String address);

    static void server_query_callback(ServerListView* self, Vector<GameServer*>* servers);
};


#endif //RAC1_MULTIPLAYER_SERVERLISTVIEW_H
