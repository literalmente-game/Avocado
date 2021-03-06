#include "dma.h"
#include <cstdio>
#include "mips.h"

namespace device {
namespace dma {
DMA::DMA(mips::CPU *cpu) : cpu(cpu) {
    dma[0] = std::make_unique<dmaChannel::DMAChannel>(0, cpu);
    dma[1] = std::make_unique<dmaChannel::DMAChannel>(1, cpu);
    dma[2] = std::make_unique<dmaChannel::DMA2Channel>(2, cpu, cpu->gpu.get());
    dma[3] = std::make_unique<dmaChannel::DMA3Channel>(3, cpu);
    dma[4] = std::make_unique<dmaChannel::DMAChannel>(4, cpu);
    dma[5] = std::make_unique<dmaChannel::DMAChannel>(5, cpu);
    dma[6] = std::make_unique<dmaChannel::DMA6Channel>(6, cpu);
}

void DMA::step() {
    bool prevMasterFlag = status.masterFlag;

    uint8_t enables = (status._reg & 0x7F0000) >> 16;
    uint8_t flags = (status._reg & 0x7F000000) >> 24;
    status.masterFlag = status.forceIRQ || (status.masterEnable && (enables & flags));

    if (!prevMasterFlag && status.masterFlag) {
        cpu->interrupt->trigger(interrupt::DMA);
    }
}

uint8_t DMA::read(uint32_t address) {
    int channel = address / 0x10;
    if (channel < 7) return dma[channel]->read(address % 0x10);

    // control
    address += 0x80;
    if (address >= 0xf0 && address < 0xf4) {
        return control._byte[address - 0xf0];
    }
    if (address >= 0xf4 && address < 0xf8) {
        return status._byte[address - 0xf4];
    }
    return 0;
}

void DMA::write(uint32_t address, uint8_t data) {
    int channel = address / 0x10;
    if (channel > 6)  // control
    {
        address += 0x80;
        if (address >= 0xF0 && address < 0xf4) {
            control._byte[address - 0xf0] = data;
            return;
        }
        if (address >= 0xF4 && address < 0xf8) {
            if (address == 0xf7) {
                // Clear flags (by writing 1 to bit) which sets it to 0
                // do not touch master flag
                status._byte[address - 0xF4] &= 0x80 | ((~data) & 0x7f);
                return;
            }
            status._byte[address - 0xf4] = data;
            return;
        }
        printf("W Unimplemented DMA address 0x%08x\n", address);
        return;
    }

    dma[channel]->write(address % 0x10, data);
    if (dma[channel]->irqFlag) {
        dma[channel]->irqFlag = false;
        if (status.getEnableDma(channel)) status.setFlagDma(channel, 1);
    }
}
}  // namespace dma
}  // namespace device
