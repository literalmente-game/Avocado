#include "gpu.h"
#include <imgui.h>
#include <cassert>
#include <cstdio>
#include <glm/glm.hpp>
#include "render.h"

const char* CommandStr[] = {"None",           "FillRectangle",  "Polygon",       "Line",           "Rectangle",
                            "CopyCpuToVram1", "CopyCpuToVram2", "CopyVramToCpu", "CopyVramToVram", "Extra"};

void GPU::reset() {
    irqRequest = false;
    displayDisable = true;
    dmaDirection = 0;
    displayAreaStartX = 0;
    displayAreaStartY = 0;
    displayRangeX1 = 0x200;
    displayRangeX2 = 0x200 + 256 * 10;
    displayRangeY1 = 0x10;
    displayRangeY2 = 0x10 + 240;

    gp1_08._reg = 0;
    gp0_e1._reg = 0;
    gp0_e2._reg = 0;

    drawingAreaLeft = 0;
    drawingAreaTop = 0;
    drawingAreaRight = 0;
    drawingAreaBottom = 0;
    drawingOffsetX = 0;
    drawingOffsetY = 0;

    gp0_e6._reg = 0;
}

void GPU::drawPolygon(int16_t x[4], int16_t y[4], RGB c[4], TextureInfo t, bool isFourVertex, bool textured, int flags) {
    int baseX = 0, baseY = 0, clutX = 0, clutY = 0, bitcount = 0;

    if (textured) {
        clutX = t.getClutX();
        clutY = t.getClutY();
        baseX = t.getBaseX();
        baseY = t.getBaseY();
        bitcount = t.getBitcount();
        flags |= (t.isTransparent()) << 6;
    }

    Vertex v[3];
    for (int i : {0, 1, 2}) {
        v[i] = {{x[i], y[i]}, {c[i].r, c[i].g, c[i].b}, {t.uv[i].x, t.uv[i].y}, bitcount, {clutX, clutY}, {baseX, baseY}, flags};
    }
    drawTriangle(this, v);

    if (isFourVertex) {
        for (int i : {1, 2, 3}) {
            v[i - 1] = {{x[i], y[i]}, {c[i].r, c[i].g, c[i].b}, {t.uv[i].x, t.uv[i].y}, bitcount, {clutX, clutY}, {baseX, baseY}, flags};
        }
        drawTriangle(this, v);
    }
}

void GPU::cmdFillRectangle(uint8_t command, uint32_t arguments[]) {
    startX = currX = arguments[1] & 0xffff;
    startY = currY = (arguments[1] & 0xffff0000) >> 16;
    endX = startX + (arguments[2] & 0xffff);
    endY = startY + ((arguments[2] & 0xffff0000) >> 16);

    uint32_t color = to15bit(arguments[0] & 0xffffff);

    for (;;) {
        VRAM[currY % GPU::VRAM_HEIGHT][currX % GPU::VRAM_WIDTH] = color;

        if (currX++ >= endX) {
            currX = startX;
            if (++currY >= endY) break;
        }
    }

    cmd = Command::None;
}

void GPU::cmdPolygon(PolygonArgs arg, uint32_t arguments[]) {
    int ptr = 1;
    int16_t x[4], y[4];
    RGB c[4] = {};
    TextureInfo tex;
    for (int i = 0; i < arg.getVertexCount(); i++) {
        x[i] = arguments[ptr] & 0xffff;
        y[i] = (arguments[ptr++] & 0xffff0000) >> 16;

        if (!arg.isRawTexture && (!arg.gouroudShading || i == 0)) c[i].c = arguments[0] & 0xffffff;
        if (arg.isTextureMapped) {
            if (i == 0) tex.palette = arguments[ptr];
            if (i == 1) tex.texpage = arguments[ptr];
            tex.uv[i].x = arguments[ptr] & 0xff;
            tex.uv[i].y = (arguments[ptr] >> 8) & 0xff;
            ptr++;
        }
        if (arg.gouroudShading && i < arg.getVertexCount() - 1) c[i + 1].c = arguments[ptr++];
    }
    int flags = 0;
    if (arg.semiTransparency) flags |= Vertex::SemiTransparency;
    if (arg.isRawTexture) flags |= Vertex::RawTexture;
    if (gp0_e1.dither24to15) flags |= Vertex::Dithering;
    drawPolygon(x, y, c, tex, arg.isQuad, arg.isTextureMapped, flags);

    cmd = Command::None;
}

void GPU::cmdLine(LineArgs arg, uint32_t arguments[]) {
    int ptr = 1;
    int16_t x[2] = {}, y[2] = {};
    RGB c[2] = {};

    for (int i = 0; i < arg.getArgumentCount() - 1; i++) {
        if (arguments[ptr] == 0x55555555) break;
        if (i == 0) {
            x[0] = arguments[ptr] & 0xffff;
            y[0] = (arguments[ptr++] & 0xffff0000) >> 16;
            c[0].c = arguments[0] & 0xffffff;
        } else {
            x[0] = x[1];
            y[0] = y[1];
            c[0] = c[1];
        }

        if (arg.gouroudShading)
            c[1].c = arguments[ptr++];
        else
            c[1].c = arguments[0] & 0xffffff;

        x[1] = arguments[ptr] & 0xffff;
        y[1] = (arguments[ptr++] & 0xffff0000) >> 16;

        // No transparency support
        // No Gouroud Shading
        drawLine(this, x, y, c);
    }

    cmd = Command::None;
}

void GPU::cmdRectangle(RectangleArgs arg, uint32_t arguments[]) {
    int w = arg.getSize();
    int h = arg.getSize();

    if (arg.size == 0) {
        w = (int32_t)(int16_t)(arguments[(arg.isTextureMapped ? 3 : 2)] & 0xffff);
        h = (int32_t)(int16_t)((arguments[(arg.isTextureMapped ? 3 : 2)] & 0xffff0000) >> 16);
    }

    int16_t x = arguments[1] & 0xffff;
    int16_t y = (arguments[1] & 0xffff0000) >> 16;

    int16_t _x[4] = {x, x + w, x, x + w};
    int16_t _y[4] = {y, y, y + h, y + h};
    RGB _c[4];
    _c[0].c = arguments[0];
    _c[1].c = arguments[0];
    _c[2].c = arguments[0];
    _c[3].c = arguments[0];
    TextureInfo tex;

    if (arg.isTextureMapped) {
        int texX = arguments[2] & 0xff;
        int texY = (arguments[2] & 0xff00) >> 8;

        tex.palette = arguments[2];
        tex.texpage = (gp0_e1._reg << 16);

        tex.uv[0].x = texX;
        tex.uv[0].y = texY;

        tex.uv[1].x = texX + w;
        tex.uv[1].y = texY;

        tex.uv[2].x = texX;
        tex.uv[2].y = texY + h;

        tex.uv[3].x = texX + w;
        tex.uv[3].y = texY + h;
    }
    int flags = 0;
    if (arg.semiTransparency) flags |= Vertex::SemiTransparency;
    if (arg.isRawTexture) flags |= Vertex::RawTexture;

    drawPolygon(_x, _y, _c, tex, true, arg.isTextureMapped, flags);

    cmd = Command::None;
}

void GPU::cmdCpuToVram1(uint8_t command, uint32_t arguments[]) {
    if ((arguments[0] & 0x00ffffff) != 0) {
        printf("cmdCpuToVram1: Suspicious arg0: 0x%x\n", arguments[0]);
    }
    startX = currX = arguments[1] & 0xffff;
    startY = currY = (arguments[1] & 0xffff0000) >> 16;

    endX = startX + (arguments[2] & 0xffff);
    endY = startY + ((arguments[2] & 0xffff0000) >> 16);

    cmd = Command::CopyCpuToVram2;
    argumentCount = 1;
    currentArgument = 0;
}

void GPU::cmdCpuToVram2(uint8_t command, uint32_t arguments[]) {
    uint32_t byte = arguments[0];

    // TODO: ugly code
    VRAM[currY % GPU::VRAM_HEIGHT][currX++ % GPU::VRAM_WIDTH] = byte & 0xffff;
    if (currX >= endX) {
        currX = startX;
        if (++currY >= endY) cmd = Command::None;
    }

    VRAM[currY % GPU::VRAM_HEIGHT][currX++ % GPU::VRAM_WIDTH] = (byte >> 16) & 0xffff;
    if (currX >= endX) {
        currX = startX;
        if (++currY >= endY) cmd = Command::None;
    }

    currentArgument = 0;
}

void GPU::cmdVramToCpu(uint8_t command, uint32_t arguments[]) {
    if ((arguments[0] & 0x00ffffff) != 0) {
        printf("cmdVramToCpu: Suspicious arg0: 0x%x\n", arguments[0]);
    }
    gpuReadMode = 1;
    startX = currX = arguments[1] & 0xffff;
    startY = currY = (arguments[1] & 0xffff0000) >> 16;
    endX = startX + (arguments[2] & 0xffff);
    endY = startY + ((arguments[2] & 0xffff0000) >> 16);

    cmd = Command::None;
}

void GPU::cmdVramToVram(uint8_t command, uint32_t arguments[]) {
    if ((arguments[0] & 0x00ffffff) != 0) {
        printf("cpuVramToVram: Suspicious arg0: 0x%x\n", arguments[0]);
    }
    int srcX = arguments[1] & 0xffff;
    int srcY = (arguments[1] & 0xffff0000) >> 16;

    int dstX = arguments[2] & 0xffff;
    int dstY = (arguments[2] & 0xffff0000) >> 16;

    int width = (arguments[3] & 0xffff);
    int height = ((arguments[3] & 0xffff0000) >> 16);

    if (width > GPU::VRAM_WIDTH || height > GPU::VRAM_HEIGHT) {
        printf("cpuVramToVram: Suspicious width: 0x%x or height: 0x%x\n", width, height);
        cmd = Command::None;
        return;
    }

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            VRAM[(dstY + y) % GPU::VRAM_HEIGHT][(dstX + x) % GPU::VRAM_WIDTH]
                = VRAM[(srcY + y) % GPU::VRAM_HEIGHT][(srcX + x) % GPU::VRAM_WIDTH];
        }
    }

    cmd = Command::None;
}

void GPU::step() {
    uint8_t dataRequest = 0;
    if (dmaDirection == 0)
        dataRequest = 0;
    else if (dmaDirection == 1)
        dataRequest = 1;  // FIFO not full
    else if (dmaDirection == 2)
        dataRequest = 1;  // Same as bit28, ready to receive dma block
    else if (dmaDirection == 3)
        dataRequest = cmd != Command::CopyCpuToVram2;  // Same as bit27, ready to send VRAM to CPU

    GPUSTAT = gp0_e1._reg & 0x7FF;
    GPUSTAT |= gp0_e6.setMaskWhileDrawing << 11;
    GPUSTAT |= gp0_e6.checkMaskBeforeDraw << 12;
    GPUSTAT |= 1 << 13;  // always set
    GPUSTAT |= (uint8_t)gp1_08.reverseFlag << 14;
    GPUSTAT |= (uint8_t)gp0_e1.textureDisable << 15;
    GPUSTAT |= (uint8_t)gp1_08.horizontalResolution2 << 16;
    GPUSTAT |= (uint8_t)gp1_08.horizontalResolution1 << 17;
    GPUSTAT |= (uint8_t)gp1_08.verticalResolution << 19;
    GPUSTAT |= (uint8_t)gp1_08.videoMode << 20;
    GPUSTAT |= (uint8_t)gp1_08.colorDepth << 21;
    GPUSTAT |= gp1_08.interlace << 22;
    GPUSTAT |= displayDisable << 23;
    GPUSTAT |= irqRequest << 24;
    GPUSTAT |= dataRequest << 25;
    GPUSTAT |= 1 << 26;  // Ready for DMA command
    GPUSTAT |= (cmd != Command::CopyCpuToVram2) << 27;
    GPUSTAT |= 1 << 28;  // Ready for receive DMA block
    GPUSTAT |= (dmaDirection & 3) << 29;
    GPUSTAT |= odd << 31;
}

uint32_t GPU::read(uint32_t address) {
    int reg = address & 0xfffffffc;
    if (reg == 0) {
        if (gpuReadMode == 0 || gpuReadMode == 2) {
            return GPUREAD;
        }
        if (gpuReadMode == 1) {
            uint32_t word = VRAM[currY][currX] | (VRAM[currY][currX + 1] << 16);
            currX += 2;

            if (currX >= endX) {
                currX = startX;
                if (++currY >= endY) {
                    gpuReadMode = 0;
                }
            }
            return word;
        }
    }
    if (reg == 4) {
        step();
        return GPUSTAT;
    }
    return 0;
}

void GPU::write(uint32_t address, uint32_t data) {
    int reg = address & 0xfffffffc;
    if (reg == 0) writeGP0(data);
    if (reg == 4) writeGP1(data);
}

void GPU::writeGP0(uint32_t data) {
    if (cmd == Command::None) {
        command = data >> 24;
        arguments[0] = data & 0xffffff;
        argumentCount = 0;
        currentArgument = 1;

        if (command == 0x00) {
            // NOP
        } else if (command == 0x01) {
            // Clear Cache
        } else if (command == 0x02) {
            // Fill rectangle
            cmd = Command::FillRectangle;
            argumentCount = 2;
        } else if (command >= 0x20 && command < 0x40) {
            // Polygons
            cmd = Command::Polygon;
            argumentCount = PolygonArgs(command).getArgumentCount();
        } else if (command >= 0x40 && command < 0x60) {
            // Lines
            cmd = Command::Line;
            argumentCount = LineArgs(command).getArgumentCount();
        } else if (command >= 0x60 && command < 0x80) {
            // Rectangles
            cmd = Command::Rectangle;
            argumentCount = RectangleArgs(command).getArgumentCount();
        } else if (command == 0xa0) {
            // Copy rectangle (CPU -> VRAM)
            cmd = Command::CopyCpuToVram1;
            argumentCount = 2;
        } else if (command == 0xc0) {
            // Copy rectangle (VRAM -> CPU)
            cmd = Command::CopyVramToCpu;
            argumentCount = 2;
        } else if (command == 0x80) {
            // Copy rectangle (VRAM -> VRAM)
            cmd = Command::CopyVramToVram;
            argumentCount = 3;
        } else if (command == 0xe1) {
            // Draw mode setting
            gp0_e1._reg = arguments[0];
        } else if (command == 0xe2) {
            // Texture window setting
            gp0_e2._reg = arguments[0];
        } else if (command == 0xe3) {
            // Drawing area top left
            drawingAreaLeft = arguments[0] & 0x3ff;
            drawingAreaTop = (arguments[0] & 0xffc00) >> 10;
        } else if (command == 0xe4) {
            // Drawing area bottom right
            drawingAreaRight = arguments[0] & 0x3ff;
            drawingAreaBottom = (arguments[0] & 0xffc00) >> 10;
        } else if (command == 0xe5) {
            // Drawing offset
            drawingOffsetX = ((int16_t)((arguments[0] & 0x7ff) << 5)) >> 5;
            drawingOffsetY = ((int16_t)(((arguments[0] & 0x3FF800) >> 11) << 5)) >> 5;
        } else if (command == 0xe6) {
            // Mask bit setting
            gp0_e6._reg = arguments[0];
        } else if (command == 0x1f) {
            // Interrupt request
            irqRequest = true;
            // TODO: IRQ
        } else {
            printf("GP0(0x%02x) args 0x%06x\n", command, arguments[0]);
        }

        if (gpuLogEnabled && cmd == Command::None) {
            GPU_LOG_ENTRY entry;
            entry.cmd = Command::Extra;
            entry.command = command;
            entry.args = std::vector<uint32_t>();
            entry.args.push_back(arguments[0]);
            gpuLogList.push_back(entry);
        }
        // if (cmd == Command::None) printf("GPU: 0x%02x\n", command);

        argumentCount++;
        return;
    }

    if (currentArgument < argumentCount) {
        arguments[currentArgument++] = data;
        if (argumentCount == MAX_ARGS && data == 0x55555555) argumentCount = currentArgument;
        if (currentArgument != argumentCount) return;
    }

    if (gpuLogEnabled && cmd != Command::CopyCpuToVram2) {
        GPU_LOG_ENTRY entry;
        entry.cmd = cmd;
        entry.command = command;
        entry.args = std::vector<uint32_t>(arguments, arguments + argumentCount);
        gpuLogList.push_back(entry);
    }

    // printf("%s(0x%x)\n", CommandStr[(int)cmd], command);

    if (cmd == Command::FillRectangle)
        cmdFillRectangle(command, arguments);
    else if (cmd == Command::Polygon)
        cmdPolygon(command, arguments);
    else if (cmd == Command::Line)
        cmdLine(command, arguments);
    else if (cmd == Command::Rectangle)
        cmdRectangle(command, arguments);
    else if (cmd == Command::CopyCpuToVram1)
        cmdCpuToVram1(command, arguments);
    else if (cmd == Command::CopyCpuToVram2)
        cmdCpuToVram2(command, arguments);
    else if (cmd == Command::CopyVramToCpu)
        cmdVramToCpu(command, arguments);
    else if (cmd == Command::CopyVramToVram)
        cmdVramToVram(command, arguments);
}

void GPU::writeGP1(uint32_t data) {
    uint32_t command = (data >> 24) & 0x3f;
    uint32_t argument = data & 0xffffff;

    if (command == 0x00) {  // Reset GPU
        reset();
    } else if (command == 0x01) {  // Reset command buffer

    } else if (command == 0x02) {  // Acknowledge IRQ1
        irqRequest = false;
    } else if (command == 0x03) {  // Display Enable
        displayDisable = (Bit)(argument & 1);
    } else if (command == 0x04) {  // DMA Direction
        dmaDirection = argument & 3;
    } else if (command == 0x05) {  // Start of display area
        displayAreaStartX = argument & 0x3ff;
        displayAreaStartY = argument >> 10;
    } else if (command == 0x06) {  // Horizontal display range
        displayRangeX1 = argument & 0xfff;
        displayRangeX2 = argument >> 12;
    } else if (command == 0x07) {  // Vertical display range
        displayRangeY1 = argument & 0x3ff;
        displayRangeX2 = argument >> 10;
    } else if (command == 0x08) {  // Display mode
        gp1_08._reg = argument;
    } else if (command == 0x09) {  // Allow texture disable
        textureDisableAllowed = argument & 1;
    } else if (command >= 0x10 && command <= 0x1f) {  // get GPU Info
        gpuReadMode = 2;
        argument &= 0xf;

        if (argument == 2) {
            GPUREAD = gp0_e2._reg;
        } else if (argument == 3) {
            GPUREAD = (drawingAreaTop << 10) | drawingAreaLeft;
        } else if (argument == 4) {
            GPUREAD = (drawingAreaBottom << 10) | drawingAreaRight;
        } else if (argument == 5) {
            GPUREAD = (drawingOffsetY << 11) | drawingOffsetX;
        } else if (argument == 7) {
            GPUREAD = 2;  // GPU Version
        } else if (argument == 8) {
            GPUREAD = 0;
        } else {
            // GPUREAD unchanged
        }
    } else {
        printf("GP1(0x%02x) args 0x%06x\n", command, argument);
        assert(false);
    }
    // command 0x20 is not implemented
}

bool GPU::emulateGpuCycles(int cycles) {
    const int LINE_VBLANK_START_NTSC = 243;
    const int LINES_TOTAL_NTSC = 263;
    static int gpuLine = 0;
    static int gpuDot = 0;

    gpuDot += cycles;

    int newLines = gpuDot / 3413;
    if (newLines == 0) return false;
    gpuDot %= 3413;
    gpuLine += newLines;

    if (gpuLine < LINE_VBLANK_START_NTSC - 1) {
        if (gp1_08.verticalResolution == GP1_08::VerticalResolution::r480 && gp1_08.interlace) {
            odd = (frames % 2) != 0;
        } else {
            odd = (gpuLine % 2) != 0;
        }
    } else {
        odd = false;
    }

    if (gpuLine == LINES_TOTAL_NTSC - 1) {
        gpuLine = 0;
        frames++;
        return true;
    }
    return false;
}