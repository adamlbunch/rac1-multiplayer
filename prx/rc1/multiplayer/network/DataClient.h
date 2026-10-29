//
// Created by bordplate on 11/24/2025.
//

#ifndef RAC1_MULTIPLAYER_DATACLIENT_H
#define RAC1_MULTIPLAYER_DATACLIENT_H

#include <sys/timer.h>
#include <sys/time.h>
#include <sys/ppu_thread.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "Packet.h"

struct DataClientConnectionInfo {
    sockaddr_in* sockaddr;
    u32 key;
};

class DataClient {
public:
    DataClient(sockaddr_in* sockaddr, u32 key);

    void _connect();
    void run();

    bool busy() {
        return should_transmit_ || is_receiving_;
    }

    static void start_data_client(DataClientConnectionInfo* connection_info) {
        DataClient client(connection_info->sockaddr, connection_info->key);
        client._connect();
        client.run();

        client.running_ = false;
        data_client_ = nullptr;

        if (client.sockfd_ >= 0) {
            socketclose(client.sockfd_);
        }

        sys_ppu_thread_exit(0);
    }

    static DataClient* shared() {
        // This is probably not good, considering the threading of it all.
        return data_client_;
    }

    bool is_running() const {
        return running_;
    }

    bool receiving() const {
        return is_receiving_;
    }

    static void transmit(MPFileType file_type, void* data, size_t len);

    // The last save file the server sent us. It's in memory of our own: it used to be written to 0x1000000, which is
    //  the game's memory and in use on some levels (Hoven's floor tiles keep their ripples there), so a save arriving
    //  while playing there crashed the game.
    static void* save_file() {
        return save_file_;
    }

    // While the game loads the save file, a save that starts arriving waits instead of being written over it. Lock
    //  fails if one is arriving already.
    static bool lock_save_file();
    static void unlock_save_file();

    static bool stop;
private:
    static u8 save_file_[0xB0000];
    static volatile bool save_file_locked_;

    int sockfd_;
    u32 key_;

    bool running_;

    volatile bool is_receiving_;

    bool should_transmit_;
    void* file_to_transmit_;
    size_t file_size_;
    size_t transmitted_;
    MPFileType transmit_file_type_;

    static DataClient* data_client_;

    sockaddr_in* sockaddr_;
};


#endif //RAC1_MULTIPLAYER_DATACLIENT_H