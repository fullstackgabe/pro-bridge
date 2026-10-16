// pro-bridge: reads gamepads with SDL3 (HIDAPI) and serves them over the DSU (CemuHook) protocol.
// Workaround for macOS GameController reporting the Switch Pro Controller left stick as D-pad over Bluetooth.

#include <SDL3/SDL.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <zlib.h>

#define DSU_PROTOCOL_VERSION 1001
#define DSU_MESSAGE_VERSION 0x100000
#define DSU_MESSAGE_PORTS 0x100001
#define DSU_MESSAGE_DATA 0x100002
#define DSU_HEADER_SIZE 16
#define DSU_CLIENT_TIMEOUT_MS 5000
#define MAXIMUM_SLOTS 4
#define MAXIMUM_CLIENTS 8
#define STANDARD_GRAVITY 9.80665f
#define RADIANS_TO_DEGREES 57.29577951f
// Gyro auto-calibration: a window counts as "still" when every gyro axis stays within this spread
// and the accelerometer barely moves, for at least this long.
#define STILL_GYRO_SPREAD_DEGREES 2.0f
#define STILL_ACCEL_DELTA_G 0.03f
#define STILL_WINDOW_MICROSECONDS 1000000
#define GYRO_DEADZONE_DEGREES 0.3f

typedef struct {
    SDL_Gamepad *gamepad;
    SDL_JoystickID joystickId;
    bool hasMotion;
    Uint32 packetNumber;
    Uint64 motionTimestampMicroseconds;
    float accelerometer[3];
    float gyroscope[3];
    char guid[33];
    float gyroBiasDegrees[3];
    Uint64 stillWindowStartMicroseconds;
    int stillSampleCount;
    double stillSum[3];
    float stillMinimum[3];
    float stillMaximum[3];
    float stillAccelStart[3];
} Slot;

typedef struct {
    struct sockaddr_in address;
    Uint64 lastRequestMilliseconds[MAXIMUM_SLOTS];
} Client;

static Slot slots[MAXIMUM_SLOTS];
static Client clients[MAXIMUM_CLIENTS];
static int serverSocket = -1;
static Uint32 serverId;
static bool verbose = false;
static bool motionEnabled = true;
static bool positionalFaceButtons = false;
static bool gyroCalibrationEnabled = true;

static void writeUint16(Uint8 *destination, Uint16 value) { memcpy(destination, &value, 2); }
static void writeUint32(Uint8 *destination, Uint32 value) { memcpy(destination, &value, 4); }
static void writeUint64(Uint8 *destination, Uint64 value) { memcpy(destination, &value, 8); }
static void writeFloat(Uint8 *destination, float value) { memcpy(destination, &value, 4); }

static Uint32 readUint32(const Uint8 *source) {
    Uint32 value;
    memcpy(&value, source, 4);
    return value;
}

// Header + CRC; payloadLength counts everything after the 16-byte header (message type included).
static void finishPacket(Uint8 *packet, Uint16 payloadLength) {
    memcpy(packet, "DSUS", 4);
    writeUint16(packet + 4, DSU_PROTOCOL_VERSION);
    writeUint16(packet + 6, payloadLength);
    writeUint32(packet + 8, 0);
    writeUint32(packet + 12, serverId);
    writeUint32(packet + 8, (Uint32)crc32(0, packet, DSU_HEADER_SIZE + payloadLength));
}

static Uint8 batteryCode(SDL_Gamepad *gamepad) {
    int percent = -1;
    switch (SDL_GetGamepadPowerInfo(gamepad, &percent)) {
    case SDL_POWERSTATE_CHARGING: return 0xEE;
    case SDL_POWERSTATE_CHARGED: return 0xEF;
    case SDL_POWERSTATE_ON_BATTERY:
        if (percent < 0) return 0x00;
        if (percent <= 10) return 0x01;
        if (percent <= 30) return 0x02;
        if (percent <= 60) return 0x03;
        if (percent <= 90) return 0x04;
        return 0x05;
    default: return 0x00;
    }
}

// The 11 bytes shared by port-info and data replies.
static void writeSlotDescription(Uint8 *destination, int slotIndex) {
    const Slot *slot = &slots[slotIndex];
    memset(destination, 0, 11);
    destination[0] = (Uint8)slotIndex;
    if (!slot->gamepad) return;
    destination[1] = 2;
    destination[2] = slot->hasMotion ? 2 : 0;
    destination[3] = SDL_GetGamepadConnectionState(slot->gamepad) == SDL_JOYSTICK_CONNECTION_WIRED ? 1 : 2;
    destination[9] = (Uint8)(slotIndex + 1);
    destination[10] = batteryCode(slot->gamepad);
}

static void sendPacket(const Uint8 *packet, size_t length, const struct sockaddr_in *address) {
    sendto(serverSocket, packet, length, 0, (const struct sockaddr *)address, sizeof(*address));
}

static Uint8 stickToByte(Sint16 value) { return (Uint8)((value + 32768) >> 8); }
static Uint8 triggerToByte(Sint16 value) { return (Uint8)(value < 0 ? 0 : value >> 7); }

static void sendSlotData(int slotIndex) {
    Slot *slot = &slots[slotIndex];
    if (!slot->gamepad) return;

    Uint64 now = SDL_GetTicks();
    bool anyClient = false;
    for (int i = 0; i < MAXIMUM_CLIENTS; i++) {
        if (clients[i].lastRequestMilliseconds[slotIndex] && now - clients[i].lastRequestMilliseconds[slotIndex] < DSU_CLIENT_TIMEOUT_MS) anyClient = true;
    }
    if (!anyClient) return;

    SDL_Gamepad *gamepad = slot->gamepad;
    Uint8 packet[DSU_HEADER_SIZE + 84] = {0};
    Uint8 *payload = packet + DSU_HEADER_SIZE;
    writeUint32(payload, DSU_MESSAGE_DATA);
    writeSlotDescription(payload + 4, slotIndex);
    payload[15] = 1;
    writeUint32(payload + 16, slot->packetNumber++);

    bool dpadLeft = SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
    bool dpadDown = SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    bool dpadRight = SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    bool dpadUp = SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_UP);
    // SDL3 buttons are positional. Astris reads DSU faces by Xbox label (bottom = A), so by default the
    // Nintendo labels are sent where Astris expects them; --positional keeps the physical positions instead.
    bool west = SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_WEST);
    bool south = SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_SOUTH);
    bool east = SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_EAST);
    bool north = SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_NORTH);
    bool buttonY = positionalFaceButtons ? west : north;
    bool buttonB = positionalFaceButtons ? south : east;
    bool buttonA = positionalFaceButtons ? east : south;
    bool buttonX = positionalFaceButtons ? north : west;
    bool buttonR = SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    bool buttonL = SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    Sint16 triggerRight = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    Sint16 triggerLeft = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    bool buttonZR = triggerRight > 16384;
    bool buttonZL = triggerLeft > 16384;

    payload[20] = (Uint8)((dpadLeft ? 0x80 : 0) | (dpadDown ? 0x40 : 0) | (dpadRight ? 0x20 : 0) | (dpadUp ? 0x10 : 0) |
                          (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_START) ? 0x08 : 0) |
                          (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_RIGHT_STICK) ? 0x04 : 0) |
                          (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_LEFT_STICK) ? 0x02 : 0) |
                          (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_BACK) ? 0x01 : 0));
    payload[21] = (Uint8)((buttonY ? 0x80 : 0) | (buttonB ? 0x40 : 0) | (buttonA ? 0x20 : 0) | (buttonX ? 0x10 : 0) |
                          (buttonR ? 0x08 : 0) | (buttonL ? 0x04 : 0) | (buttonZR ? 0x02 : 0) | (buttonZL ? 0x01 : 0));
    payload[22] = SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_GUIDE);
    payload[23] = SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_MISC1);

    // DSU sticks: 0..255, centre 128, Y positive = up (SDL is Y positive = down).
    payload[24] = stickToByte(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX));
    payload[25] = (Uint8)(255 - stickToByte(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY)));
    payload[26] = stickToByte(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTX));
    payload[27] = (Uint8)(255 - stickToByte(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTY)));

    payload[28] = dpadLeft ? 255 : 0;
    payload[29] = dpadDown ? 255 : 0;
    payload[30] = dpadRight ? 255 : 0;
    payload[31] = dpadUp ? 255 : 0;
    payload[32] = buttonY ? 255 : 0;
    payload[33] = buttonB ? 255 : 0;
    payload[34] = buttonA ? 255 : 0;
    payload[35] = buttonX ? 255 : 0;
    payload[36] = buttonR ? 255 : 0;
    payload[37] = buttonL ? 255 : 0;
    payload[38] = triggerToByte(triggerRight);
    payload[39] = triggerToByte(triggerLeft);

    // DSU motion uses the same axes as SDL (x right, y up, z toward player): accel in g, gyro in deg/s.
    if (slot->hasMotion) {
        writeUint64(payload + 52, slot->motionTimestampMicroseconds);
        for (int axis = 0; axis < 3; axis++) {
            writeFloat(payload + 60 + axis * 4, slot->accelerometer[axis] / STANDARD_GRAVITY);
            float degrees = slot->gyroscope[axis] * RADIANS_TO_DEGREES - slot->gyroBiasDegrees[axis];
            if (SDL_fabsf(degrees) < GYRO_DEADZONE_DEGREES) degrees = 0;
            writeFloat(payload + 72 + axis * 4, degrees);
        }
    }

    finishPacket(packet, 84);
    for (int i = 0; i < MAXIMUM_CLIENTS; i++) {
        Uint64 last = clients[i].lastRequestMilliseconds[slotIndex];
        if (last && now - last < DSU_CLIENT_TIMEOUT_MS) sendPacket(packet, sizeof(packet), &clients[i].address);
    }
}

static Client *findOrAddClient(const struct sockaddr_in *address) {
    Uint64 now = SDL_GetTicks();
    Client *freeClient = NULL;
    for (int i = 0; i < MAXIMUM_CLIENTS; i++) {
        Client *client = &clients[i];
        if (client->address.sin_port == address->sin_port && client->address.sin_addr.s_addr == address->sin_addr.s_addr) return client;
        bool expired = true;
        for (int slotIndex = 0; slotIndex < MAXIMUM_SLOTS; slotIndex++) {
            if (client->lastRequestMilliseconds[slotIndex] && now - client->lastRequestMilliseconds[slotIndex] < DSU_CLIENT_TIMEOUT_MS) expired = false;
        }
        if (expired && !freeClient) freeClient = client;
    }
    if (!freeClient) return NULL;
    memset(freeClient, 0, sizeof(*freeClient));
    freeClient->address = *address;
    if (verbose) SDL_Log("DSU client %s:%d subscribed", inet_ntoa(address->sin_addr), ntohs(address->sin_port));
    return freeClient;
}

static void handleRequest(const Uint8 *packet, ssize_t length, const struct sockaddr_in *address) {
    if (length < DSU_HEADER_SIZE + 4 || memcmp(packet, "DSUC", 4) != 0) return;
    const Uint8 *payload = packet + DSU_HEADER_SIZE;
    Uint32 messageType = readUint32(payload);
    if (verbose && messageType != DSU_MESSAGE_DATA) SDL_Log("DSU request 0x%X from %s:%d", messageType, inet_ntoa(address->sin_addr), ntohs(address->sin_port));

    if (messageType == DSU_MESSAGE_VERSION) {
        Uint8 reply[DSU_HEADER_SIZE + 8] = {0};
        writeUint32(reply + DSU_HEADER_SIZE, DSU_MESSAGE_VERSION);
        writeUint16(reply + DSU_HEADER_SIZE + 4, DSU_PROTOCOL_VERSION);
        finishPacket(reply, 8);
        sendPacket(reply, sizeof(reply), address);
    } else if (messageType == DSU_MESSAGE_PORTS && length >= DSU_HEADER_SIZE + 8) {
        Sint32 count = (Sint32)readUint32(payload + 4);
        for (Sint32 i = 0; i < count && i < MAXIMUM_SLOTS && DSU_HEADER_SIZE + 8 + i < length; i++) {
            Uint8 slotIndex = payload[8 + i];
            if (slotIndex >= MAXIMUM_SLOTS) continue;
            Uint8 reply[DSU_HEADER_SIZE + 16] = {0};
            writeUint32(reply + DSU_HEADER_SIZE, DSU_MESSAGE_PORTS);
            writeSlotDescription(reply + DSU_HEADER_SIZE + 4, slotIndex);
            finishPacket(reply, 16);
            sendPacket(reply, sizeof(reply), address);
        }
    } else if (messageType == DSU_MESSAGE_DATA && length >= DSU_HEADER_SIZE + 12) {
        Client *client = findOrAddClient(address);
        if (!client) return;
        Uint8 flags = payload[4];
        Uint8 requestedSlot = payload[5];
        const Uint8 *requestedMac = payload + 6;
        Uint64 now = SDL_GetTicks();
        for (int slotIndex = 0; slotIndex < MAXIMUM_SLOTS; slotIndex++) {
            bool bySlot = (flags & 1) && requestedSlot == slotIndex;
            bool byMac = (flags & 2) && requestedMac[5] == slotIndex + 1;
            if (flags == 0 || bySlot || byMac) client->lastRequestMilliseconds[slotIndex] = now;
        }
    }
}

// Calibration is saved per controller model+GUID so the next session starts already corrected.
static bool calibrationPath(const Slot *slot, char *path, size_t size) {
    char *directory = SDL_GetPrefPath("fullstackgabe", "pro-bridge");
    if (!directory) return false;
    SDL_snprintf(path, size, "%sgyro-%s.txt", directory, slot->guid);
    SDL_free(directory);
    return true;
}

static void loadGyroCalibration(Slot *slot) {
    char path[1024];
    if (!calibrationPath(slot, path, sizeof(path))) return;
    FILE *file = fopen(path, "r");
    if (!file) return;
    float bias[3];
    if (fscanf(file, "%f %f %f", &bias[0], &bias[1], &bias[2]) == 3) {
        memcpy(slot->gyroBiasDegrees, bias, sizeof(bias));
        SDL_Log("Gyro calibration loaded: %+.2f %+.2f %+.2f deg/s", bias[0], bias[1], bias[2]);
    }
    fclose(file);
}

static void saveGyroCalibration(const Slot *slot) {
    char path[1024];
    if (!calibrationPath(slot, path, sizeof(path))) return;
    FILE *file = fopen(path, "w");
    if (!file) return;
    fprintf(file, "%f %f %f\n", slot->gyroBiasDegrees[0], slot->gyroBiasDegrees[1], slot->gyroBiasDegrees[2]);
    fclose(file);
}

static void resetStillWindow(Slot *slot, Uint64 timestampMicroseconds) {
    slot->stillWindowStartMicroseconds = timestampMicroseconds;
    slot->stillSampleCount = 0;
    for (int axis = 0; axis < 3; axis++) {
        slot->stillSum[axis] = 0;
        slot->stillMinimum[axis] = 1e9f;
        slot->stillMaximum[axis] = -1e9f;
        slot->stillAccelStart[axis] = slot->accelerometer[axis] / STANDARD_GRAVITY;
    }
}

// While the controller rests still for a full window, its average gyro reading is pure bias.
static void updateGyroCalibration(Slot *slot) {
    Uint64 now = slot->motionTimestampMicroseconds;
    if (slot->stillSampleCount == 0) resetStillWindow(slot, now);

    for (int axis = 0; axis < 3; axis++) {
        float degrees = slot->gyroscope[axis] * RADIANS_TO_DEGREES;
        float accelDelta = SDL_fabsf(slot->accelerometer[axis] / STANDARD_GRAVITY - slot->stillAccelStart[axis]);
        slot->stillMinimum[axis] = SDL_min(slot->stillMinimum[axis], degrees);
        slot->stillMaximum[axis] = SDL_max(slot->stillMaximum[axis], degrees);
        if (slot->stillMaximum[axis] - slot->stillMinimum[axis] > STILL_GYRO_SPREAD_DEGREES || accelDelta > STILL_ACCEL_DELTA_G) {
            resetStillWindow(slot, now);
            return;
        }
        slot->stillSum[axis] += degrees;
    }
    slot->stillSampleCount++;

    if (now - slot->stillWindowStartMicroseconds < STILL_WINDOW_MICROSECONDS || slot->stillSampleCount < 50) return;

    float change = 0;
    for (int axis = 0; axis < 3; axis++) {
        float bias = (float)(slot->stillSum[axis] / slot->stillSampleCount);
        change = SDL_max(change, SDL_fabsf(bias - slot->gyroBiasDegrees[axis]));
        slot->gyroBiasDegrees[axis] = bias;
    }
    if (change > 0.2f) {
        SDL_Log("Gyro calibrated (controller still): %+.2f %+.2f %+.2f deg/s", slot->gyroBiasDegrees[0], slot->gyroBiasDegrees[1],
                slot->gyroBiasDegrees[2]);
        saveGyroCalibration(slot);
    }
    slot->stillSampleCount = 0;
}

static int slotForJoystick(SDL_JoystickID joystickId) {
    for (int i = 0; i < MAXIMUM_SLOTS; i++) {
        if (slots[i].gamepad && slots[i].joystickId == joystickId) return i;
    }
    return -1;
}

static void addGamepad(SDL_JoystickID joystickId) {
    if (slotForJoystick(joystickId) >= 0) return;
    for (int i = 0; i < MAXIMUM_SLOTS; i++) {
        if (slots[i].gamepad) continue;
        SDL_Gamepad *gamepad = SDL_OpenGamepad(joystickId);
        if (!gamepad) {
            SDL_Log("Could not open gamepad: %s", SDL_GetError());
            return;
        }
        memset(&slots[i], 0, sizeof(slots[i]));
        slots[i].gamepad = gamepad;
        slots[i].joystickId = joystickId;
        if (motionEnabled && SDL_GamepadHasSensor(gamepad, SDL_SENSOR_GYRO) && SDL_GamepadHasSensor(gamepad, SDL_SENSOR_ACCEL)) {
            slots[i].hasMotion = SDL_SetGamepadSensorEnabled(gamepad, SDL_SENSOR_GYRO, true) &&
                                 SDL_SetGamepadSensorEnabled(gamepad, SDL_SENSOR_ACCEL, true);
        }
        SDL_GUIDToString(SDL_GetGamepadGUIDForID(joystickId), slots[i].guid, sizeof(slots[i].guid));
        if (slots[i].hasMotion) loadGyroCalibration(&slots[i]);
        bool wired = SDL_GetGamepadConnectionState(gamepad) == SDL_JOYSTICK_CONNECTION_WIRED;
        SDL_Log("Slot %d: %s (%s, motion %s)", i + 1, SDL_GetGamepadName(gamepad), wired ? "USB" : "Bluetooth",
                slots[i].hasMotion ? "on" : "off");
        return;
    }
    SDL_Log("All %d DSU slots are in use; ignoring new gamepad", MAXIMUM_SLOTS);
}

static void removeGamepad(SDL_JoystickID joystickId) {
    int slotIndex = slotForJoystick(joystickId);
    if (slotIndex < 0) return;
    SDL_Log("Slot %d: disconnected", slotIndex + 1);
    SDL_CloseGamepad(slots[slotIndex].gamepad);
    memset(&slots[slotIndex], 0, sizeof(slots[slotIndex]));
}

static bool openServerSocket(const char *host, int port) {
    serverSocket = socket(AF_INET, SOCK_DGRAM, 0);
    if (serverSocket < 0) {
        perror("socket");
        return false;
    }
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &address.sin_addr) != 1) {
        fprintf(stderr, "Invalid host: %s\n", host);
        return false;
    }
    if (bind(serverSocket, (struct sockaddr *)&address, sizeof(address)) < 0) {
        fprintf(stderr, "Could not listen on %s:%d: %s\n", host, port, strerror(errno));
        return false;
    }
    fcntl(serverSocket, F_SETFL, O_NONBLOCK);
    return true;
}

static void printUsage(const char *program) {
    printf("Usage: %s [--host 127.0.0.1] [--port 26760] [--no-motion] [--no-gyro-calibration] [--positional] [--verbose]\n", program);
}

int main(int argc, char **argv) {
    const char *host = "127.0.0.1";
    int port = 26760;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--host") && i + 1 < argc) host = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-motion")) motionEnabled = false;
        else if (!strcmp(argv[i], "--verbose")) verbose = true;
        else if (!strcmp(argv[i], "--positional")) positionalFaceButtons = true;
        else if (!strcmp(argv[i], "--no-gyro-calibration")) gyroCalibrationEnabled = false;
        else {
            printUsage(argv[0]);
            return strcmp(argv[i], "--help") ? 1 : 0;
        }
    }

    // Read the controller directly over HID (the path that works), never through Apple's GameController,
    // and keep receiving input while the emulator has focus.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_MFI, "0");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    if (!openServerSocket(host, port)) return 1;
    serverId = (Uint32)SDL_rand_bits();
    SDL_Log("pro-bridge DSU server on %s:%d — waiting for controllers (Ctrl+C to quit)", host, port);

    Uint64 lastVerboseLog = 0;
    bool running = true;
    while (running) {
        SDL_Event event;
        if (SDL_WaitEventTimeout(&event, 2)) {
            do {
                switch (event.type) {
                case SDL_EVENT_QUIT: running = false; break;
                case SDL_EVENT_GAMEPAD_ADDED: addGamepad(event.gdevice.which); break;
                case SDL_EVENT_GAMEPAD_REMOVED: removeGamepad(event.gdevice.which); break;
                case SDL_EVENT_GAMEPAD_AXIS_MOTION:
                case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                case SDL_EVENT_GAMEPAD_BUTTON_UP: {
                    int slotIndex = slotForJoystick(event.gdevice.which);
                    if (slotIndex >= 0) sendSlotData(slotIndex);
                    break;
                }
                case SDL_EVENT_GAMEPAD_SENSOR_UPDATE: {
                    int slotIndex = slotForJoystick(event.gsensor.which);
                    if (slotIndex < 0) break;
                    Slot *slot = &slots[slotIndex];
                    if (event.gsensor.sensor == SDL_SENSOR_ACCEL) {
                        memcpy(slot->accelerometer, event.gsensor.data, sizeof(slot->accelerometer));
                    } else if (event.gsensor.sensor == SDL_SENSOR_GYRO) {
                        memcpy(slot->gyroscope, event.gsensor.data, sizeof(slot->gyroscope));
                        slot->motionTimestampMicroseconds = event.gsensor.sensor_timestamp / 1000;
                        if (gyroCalibrationEnabled) updateGyroCalibration(slot);
                        sendSlotData(slotIndex);
                    }
                    break;
                }
                default: break;
                }
            } while (SDL_PollEvent(&event));
        }

        Uint8 buffer[256];
        struct sockaddr_in address;
        socklen_t addressLength = sizeof(address);
        ssize_t received;
        while ((received = recvfrom(serverSocket, buffer, sizeof(buffer), 0, (struct sockaddr *)&address, &addressLength)) > 0) {
            handleRequest(buffer, received, &address);
            addressLength = sizeof(address);
        }

        if (verbose && slots[0].gamepad && SDL_GetTicks() - lastVerboseLog > 500) {
            lastVerboseLog = SDL_GetTicks();
            SDL_Log("Slot 1 left stick x=%6d y=%6d | right stick x=%6d y=%6d",
                    SDL_GetGamepadAxis(slots[0].gamepad, SDL_GAMEPAD_AXIS_LEFTX), SDL_GetGamepadAxis(slots[0].gamepad, SDL_GAMEPAD_AXIS_LEFTY),
                    SDL_GetGamepadAxis(slots[0].gamepad, SDL_GAMEPAD_AXIS_RIGHTX), SDL_GetGamepadAxis(slots[0].gamepad, SDL_GAMEPAD_AXIS_RIGHTY));
        }
    }

    for (int i = 0; i < MAXIMUM_SLOTS; i++) {
        if (slots[i].gamepad) SDL_CloseGamepad(slots[i].gamepad);
    }
    close(serverSocket);
    SDL_Quit();
    return 0;
}
