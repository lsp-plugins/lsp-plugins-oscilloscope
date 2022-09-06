/*
 * Copyright (C) 2021 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2021 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-plugins-oscilloscope
 * Created on: 3 авг. 2021 г.
 *
 * lsp-plugins-oscilloscope is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * lsp-plugins-oscilloscope is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with lsp-plugins-oscilloscope. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef PRIVATE_META_OSCILLOSCOPE_H_
#define PRIVATE_META_OSCILLOSCOPE_H_

#include <lsp-plug.in/plug-fw/meta/types.h>
#include <lsp-plug.in/plug-fw/const.h>


namespace lsp
{
    namespace meta
    {
        struct oscilloscope_metadata
        {
            static constexpr float HORIZONTAL_DIVISION_MAX      = 10.0f;
            static constexpr float HORIZONTAL_DIVISION_MIN      = 1.0e-3f;
            static constexpr float HORIZONTAL_DIVISION_DFL      = 0.5;
            static constexpr float HORIZONTAL_DIVISION_STEP     = 1e-3f;

            static constexpr float TIME_DIVISION_MAX            = 50.0f;
            static constexpr float TIME_DIVISION_MIN            = 0.05f;
            static constexpr float TIME_DIVISION_DFL            = 1.0f;
            static constexpr float TIME_DIVISION_STEP           = 0.01f;

            static constexpr float TIME_POSITION_MAX            = 100.0f;
            static constexpr float TIME_POSITION_MIN            = -100.0f;
            static constexpr float TIME_POSITION_DFL            = 0.0f;
            static constexpr float TIME_POSITION_STEP           = 0.1f;

            static constexpr float VERTICAL_DIVISION_MAX        = 10.0f;
            static constexpr float VERTICAL_DIVISION_MIN        = 1.0e-3f;
            static constexpr float VERTICAL_DIVISION_DFL        = 0.5;
            static constexpr float VERTICAL_DIVISION_STEP       = 1e-3f;

            static constexpr float VERTICAL_POSITION_MAX        = 100.0f;
            static constexpr float VERTICAL_POSITION_MIN        = -100.0f;
            static constexpr float VERTICAL_POSITION_DFL        = 0.0f;
            static constexpr float VERTICAL_POSITION_STEP       = 0.1f;

            static constexpr size_t STROBE_HISTORY_MAX          = 10;
            static constexpr size_t STROBE_HISTORY_MIN          = 0;
            static constexpr size_t STROBE_HISTORY_DFL          = 0;
            static constexpr size_t STROBE_HISTORY_STEP         = 1;

            static constexpr float XY_RECORD_TIME_MAX           = 50.0f;
            static constexpr float XY_RECORD_TIME_MIN           = 1.0f;
            static constexpr float XY_RECORD_TIME_DFL           = 10.0f;
            static constexpr float XY_RECORD_TIME_STEP          = 0.01f;

            static constexpr float MAXDOTS_MAX                  = 16384.0f;
            static constexpr float MAXDOTS_MIN                  = 512.0f;
            static constexpr float MAXDOTS_DFL                  = 8192.0f;
            static constexpr float MAXDOTS_STEP                 = 0.01f;

            static constexpr float TRIGGER_HYSTERESIS_MAX       = 50.0f;
            static constexpr float TRIGGER_HYSTERESIS_MIN       = 0.0f;
            static constexpr float TRIGGER_HYSTERESIS_DFL       = 1.0f;
            static constexpr float TRIGGER_HYSTERESIS_STEP      = 0.01f;

            static constexpr float TRIGGER_LEVEL_MAX            = 100.0f;
            static constexpr float TRIGGER_LEVEL_MIN            = -100.0f;
            static constexpr float TRIGGER_LEVEL_DFL            = 0.0f;
            static constexpr float TRIGGER_LEVEL_STEP           = 0.01f;

            static constexpr float TRIGGER_HOLD_TIME_MAX        = 60.0f;
            static constexpr float TRIGGER_HOLD_TIME_MIN        = 0.0f;
            static constexpr float TRIGGER_HOLD_TIME_DFL        = 0.0f;
            static constexpr float TRIGGER_HOLD_TIME_STEP       = 0.01f;

            enum oversampler_mode_selector_t
            {
                OSC_OVS_NONE,
                OSC_OVS_2X,
                OSC_OVS_3X,
                OSC_OVS_4X,
                OSC_OVS_6X,
                OSC_OVS_8X,

                OSC_OVS_DFL = OSC_OVS_8X
            };

            enum mode_selector_t
            {
                MODE_XY,
                MODE_TRIGGERED,
                MODE_GONIOMETER,

                MODE_DFL = MODE_TRIGGERED
            };

            enum sweep_type_selector_t
            {
                SWEEP_TYPE_SAWTOOTH,
                SWEEP_TYPE_TRIANGULAR,
                SWEEP_TYPE_SINE,

                SWEEP_TYPE_DFL = SWEEP_TYPE_SAWTOOTH
            };

            enum trigger_input_selector_t
            {
                TRIGGER_INPUT_Y,
                TRIGGER_INPUT_EXT,

                TRIGGER_INPUT_DFL = TRIGGER_INPUT_Y
            };

            enum trigger_mode_selector_t
            {
                TRIGGER_MODE_SINGLE,
                TRIGGER_MODE_MANUAL,
                TRIGGER_MODE_REPEAT,

                TRIGGER_MODE_DFL = TRIGGER_MODE_REPEAT
            };

            enum trigger_type_selector_t
            {
                TRIGGER_TYPE_NONE,
                TRIGGER_TYPE_SIMPLE_RISING_EDGE,
                TRIGGER_TYPE_SIMPE_FALLING_EDGE,
                TRIGGER_TYPE_ADVANCED_RISING_EDGE,
                TRIGGER_TYPE_ADVANCED_FALLING_EDGE,

                TRIGGER_TYPE_DFL = TRIGGER_TYPE_ADVANCED_RISING_EDGE
            };

            enum coupling_type_t
            {
                COUPLING_AC,
                COUPLING_DC,

                COUPLING_DFL = COUPLING_DC
            };

            static constexpr size_t SCOPE_MESH_SIZE = 512;
        };

        extern const meta::plugin_t oscilloscope_x1;
        extern const meta::plugin_t oscilloscope_x2;
        extern const meta::plugin_t oscilloscope_x4;
    } // namespace meta
} // namespace lsp


#endif /* PRIVATE_META_OSCILLOSCOPE_H_ */
