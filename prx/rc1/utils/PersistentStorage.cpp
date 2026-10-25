//
// Created by bordplate on 6/17/2023.
//

#include "PersistentStorage.h"

// Settings live in the mod's own folder, which exists whichever version of the game it runs with. They used to be
//  kept in the European PSN game's folder, so the disc and US versions couldn't save them and asked for the username
//  and server IP on every launch. Settings saved there before are still read.
static const char* storage_path = "/dev_hdd0/game/BORD00001/USRDIR/%s";
static const char* legacy_storage_path = "/dev_hdd0/game/NPEA00385/USRDIR/%s";

void PersistentStorage::persist() {
    int err = cellFsOpen(String::format(storage_path, filename.c_str()).c_str(), CELL_FS_O_RDWR|CELL_FS_O_TRUNC|CELL_FS_O_CREAT, &file_descriptor_, nullptr, 0);
    if (err != 0) {
        Logger::error("Couldn't save %s: 0x%08x", filename.c_str(), err);
        return;
    }

    Logger::debug("Persiting %d keys", keys_.size());

    for (int i = 0; i < keys_.size(); i++) {
        String key = keys_[i];
        String value = values_[i];

        Logger::debug("Key: %s", key.c_str());

        String line = String::format("%s=%s\n", key.c_str(), value.c_str());

        uint64_t num_write;
        cellFsWrite(file_descriptor_, line.c_str(), line.length(), &num_write);
    }

    Logger::debug("That's persitaance");

    cellFsClose(file_descriptor_);
}

void PersistentStorage::load() {
    if (!load_from(storage_path)) {
        load_from(legacy_storage_path);
    }
}

bool PersistentStorage::load_from(const char* path) {
    int err = cellFsOpen(String::format(path, filename.c_str()).c_str(), CELL_FS_O_RDONLY, &file_descriptor_, nullptr, 0);
    if (err != 0) {
        return false;
    }

    char buffer[4096];
    uint64_t num_read = 0;
    cellFsRead(file_descriptor_, (void*)buffer, sizeof(buffer) - 1, &num_read);
    cellFsClose(file_descriptor_);

    buffer[num_read] = '\0';
    if (num_read == 0) {
        return false;
    }

    Vector<String> parts = String(buffer).split("\n");
    for (int i = 0; i < parts.size(); i++) {
        Vector<String> key_value = parts[i].split("=");
        if (key_value.size() < 2) {
            continue;
        }

        String key = key_value[0];
        String value = key_value[1];

        if (value[0] == '"') {
            // This is a string value
            set(key, value.slice(1, (int)value.length()-1));
        } else if (value == "true" || value == "false") {
            set(key, value == "true");
        } else {
            // Treat as integer
            set(key, __atoi(value.c_str()));
        }
    }

    return true;
}