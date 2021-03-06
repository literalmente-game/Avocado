#pragma once
#include <deque>
#include <memory>
#include "device.h"
#include "utils/cue/cue.h"

namespace device {
namespace cdrom {

class CDROM {
    union StatusCode {
        enum class Mode { None, Reading, Seeking, Playing };
        struct {
            uint8_t error : 1;
            uint8_t motor : 1;
            uint8_t seekError : 1;
            uint8_t idError : 1;
            uint8_t shellOpen : 1;
            uint8_t read : 1;
            uint8_t seek : 1;
            uint8_t play : 1;
        };
        uint8_t _reg;

        void setMode(Mode mode) {
            error = seekError = idError = false;
            read = seek = play = false;
            motor = true;
            if (mode == Mode::Reading) {
                read = true;
            } else if (mode == Mode::Seeking) {
                seek = true;
            } else if (mode == Mode::Playing) {
                play = true;
            }
        }

        void setShell(bool opened) {
            shellOpen = opened;
            if (!shellOpen) {
                setMode(Mode::None);
            }
        }

        bool getShell() const { return shellOpen; }

        void toggleShell() {
            if (!shellOpen) {
                shellOpen = true;
                setMode(Mode::None);
            } else {
                shellOpen = false;
            }
        }

        StatusCode() : _reg(0) { shellOpen = true; }
    };

    union CDROM_Status {
        struct {
            uint8_t index : 2;
            uint8_t xaFifoEmpty : 1;
            uint8_t parameterFifoEmpty : 1;  // triggered before writing first byte
            uint8_t parameterFifoFull : 1;   // triggered after writing 16 bytes
            uint8_t responseFifoEmpty : 1;   // triggered after reading last byte
            uint8_t dataFifoEmpty : 1;       // triggered after reading last byte
            uint8_t transmissionBusy : 1;
        };
        uint8_t _reg;

        CDROM_Status() : _reg(0x18) {}
    };

    int verbose = 1;

    CDROM_Status status;
    uint8_t interruptEnable = 0;
    std::deque<uint8_t> CDROM_params;
    std::deque<uint8_t> CDROM_response;
    std::deque<uint8_t> CDROM_interrupt;

    bool sectorSize = false;  // 0 - 0x800, 1 - 0x924
    bool report = false;      // generate report on playback?

    mips::CPU *cpu = nullptr;
    int readSector = 0;

    StatusCode stat;

    void cmdGetstat();
    void cmdSetloc();
    void cmdPlay();
    void cmdReadN();
    void cmdMotorOn();
    void cmdStop();
    void cmdPause();
    void cmdInit();
    void cmdMute();
    void cmdDemute();
    void cmdSetFilter();
    void cmdSetmode();
    void cmdGetlocL();
    void cmdGetlocP();
    void cmdGetTN();
    void cmdGetTD();
    void cmdSeekL();
    void cmdTest();
    void cmdGetId();
    void cmdReadS();
    void cmdReadTOC();
    void cmdUnlock();
    void cmdSetSession();
    void cmdSeekP();
    void handleCommand(uint8_t cmd);

    void writeResponse(uint8_t byte) {
        if (CDROM_response.size() >= 16) {
            return;
        }
        CDROM_response.push_back(byte);
        status.responseFifoEmpty = 1;
    }

    uint8_t readParam() {
        uint8_t param = CDROM_params.front();
        CDROM_params.pop_front();

        status.parameterFifoEmpty = CDROM_params.empty();
        status.parameterFifoFull = 1;

        return param;
    }

   public:
    utils::Cue cue;

    CDROM(mips::CPU *cpu);
    void step();
    uint8_t read(uint32_t address);
    void write(uint32_t address, uint8_t data);

    void setShell(bool opened) { stat.setShell(opened); }
    bool getShell() const { return stat.getShell(); }
    void toggleShell() { stat.toggleShell(); }
    void ackMoreData() {
        status.dataFifoEmpty = 1;

        CDROM_interrupt.push_back(1);
        writeResponse(stat._reg);
    }
};
}  // namespace cdrom
}  // namespace device
