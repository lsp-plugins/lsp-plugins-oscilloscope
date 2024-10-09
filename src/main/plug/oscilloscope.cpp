/*
 * Copyright (C) 2023 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2023 Vladimir Sadovnikov <sadko4u@gmail.com>
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

#include <private/plugins/oscilloscope.h>
#include <lsp-plug.in/common/alloc.h>
#include <lsp-plug.in/common/debug.h>
#include <lsp-plug.in/dsp/dsp.h>
#include <lsp-plug.in/dsp-units/units.h>
#include <lsp-plug.in/shared/debug.h>
#include <lsp-plug.in/shared/id_colors.h>
#include <lsp-plug.in/stdlib/math.h>

#define BUF_LIM_SIZE        196608
#define PRE_TRG_MAX_SIZE    196608

#define SWEEP_GEN_N_BITS    32
#define SWEEP_GEN_PEAK      1.0f    /* Stream min x coordinate should be -SWEEP_GEN_PEAK and max x coordinate should be +SWEEP_GEN_PEAK */

#define DC_BLOCK_CUTOFF_HZ  5.0
#define DC_BLOCK_DFL_ALPHA  0.999f

#define STREAM_MAX_X        1.0f
#define STREAM_MIN_X       -1.0f
#define STREAM_MAX_Y        1.0f
#define STREAM_MIN_Y       -1.0f
#define STREAM_N_VER_DIV    4
#define STREAM_N_HOR_DIV    4
#define DECIM_PRECISION     0.1e-5  /* For development, this should be calculated from screen size */
#define IDISPLAY_DECIM      0.2e-2  /* Decimation for inline display */

#define AUTO_SWEEP_TIME     1.0f


namespace lsp
{
    namespace plugins
    {
        //-------------------------------------------------------------------------
        // Plugin factory
        inline namespace
        {
            typedef struct plugin_settings_t
            {
                const meta::plugin_t   *metadata;
                uint8_t                 channels;
            } plugin_settings_t;

            static const meta::plugin_t *plugins[] =
            {
                &meta::oscilloscope_x1,
                &meta::oscilloscope_x2,
                &meta::oscilloscope_x4
            };

            static const plugin_settings_t plugin_settings[] =
            {
                { &meta::oscilloscope_x1,       1   },
                { &meta::oscilloscope_x2,       2   },
                { &meta::oscilloscope_x4,       4   },
                { NULL, 0 }
            };

            static plug::Module *plugin_factory(const meta::plugin_t *meta)
            {
                for (const plugin_settings_t *s = plugin_settings; s->metadata != NULL; ++s)
                    if (s->metadata == meta)
                        return new oscilloscope(s->metadata, s->channels);
                return NULL;
            }

            static plug::Factory factory(plugin_factory, plugins, 3);
        } /* inline namespace */

        //-------------------------------------------------------------------------
        oscilloscope::oscilloscope(const meta::plugin_t *metadata, size_t channels): plug::Module(metadata)
        {
            sDCBlockParams.fAlpha   = 0.0f;
            sDCBlockParams.fGain    = 0.0f;

            nChannels           = channels;
            vChannels           = NULL;

            pData               = NULL;

            pStrobeHistSize     = NULL;
            pXYRecordTime       = NULL;
            pFreeze             = NULL;

            pChannelSelector    = NULL;

            pOvsMode            = NULL;
            pScpMode            = NULL;
            pCoupling_x         = NULL;
            pCoupling_y         = NULL;
            pCoupling_ext       = NULL;

            pSweepType          = NULL;
            pTimeDiv            = NULL;
            pHorDiv             = NULL;
            pHorPos             = NULL;

            pVerDiv             = NULL;
            pVerPos             = NULL;

            pTrgHys             = NULL;
            pTrgLev             = NULL;
            pTrgHold            = NULL;
            pTrgMode            = NULL;
            pTrgType            = NULL;
            pTrgInput           = NULL;
            pTrgReset           = NULL;

            pIDisplay           = NULL;
        }

        oscilloscope::~oscilloscope()
        {
            do_destroy();
        }

        void oscilloscope::destroy()
        {
            plug::Module::destroy();
            do_destroy();
        }

        void oscilloscope::do_destroy()
        {
            free_aligned(pData);
            pData = NULL;

            if (vChannels != NULL)
            {
                for (size_t ch = 0; ch < nChannels; ++ch)
                {
                    channel_t *c = &vChannels[ch];

                    c->sDCBlockBank_x.destroy();
                    c->sDCBlockBank_y.destroy();
                    c->sDCBlockBank_ext.destroy();

                    c->sOversampler_x.destroy();
                    c->sOversampler_y.destroy();
                    c->sOversampler_ext.destroy();

                    c->sPreTrgDelay.destroy();

                    c->sSweepGenerator.destroy();

                    c->vTemp            = NULL;
                    c->vData_x          = NULL;
                    c->vData_y          = NULL;
                    c->vData_ext        = NULL;
                    c->vData_y_delay    = NULL;
                    c->vDisplay_x       = NULL;
                    c->vDisplay_y       = NULL;
                    c->vDisplay_s       = NULL;

                    c->vIDisplay_x      = NULL;
                    c->vIDisplay_y      = NULL;
                }

                delete [] vChannels;
                vChannels = NULL;
            }

            if (pIDisplay != NULL)
            {
                pIDisplay->destroy();
                pIDisplay   = NULL;
            }
        }

        void oscilloscope::init(plug::IWrapper *wrapper, plug::IPort **ports)
        {
            plug::Module::init(wrapper, ports);

            vChannels = new channel_t[nChannels];
            if (vChannels == NULL)
                return;

            /** For each channel:
             * 1X temp buffer +
             * 1X external data buffer +
             * 1X x data buffer +
             * 1X y data buffer +
             * 1X delayed y data buffer +
             * 1X x display buffer +
             * 1X y display buffer +
             * 1X strobe display buffer
             * 1X x inline display buffer
             * 1X y inline display buffer
             *
             * All buffers size BUF_LIM_SIZE
             */
            size_t samples = nChannels * BUF_LIM_SIZE * 10;

            float *ptr = alloc_aligned<float>(pData, samples);
            if (ptr == NULL)
                return;

            lsp_guard_assert(float *save = ptr);

            for (size_t ch = 0; ch < nChannels; ++ch)
            {
                channel_t *c = &vChannels[ch];

                init_state_stage(c);

                if (!c->sDCBlockBank_x.init(FILTER_CHAINS_MAX))
                    return;

                if (!c->sDCBlockBank_y.init(FILTER_CHAINS_MAX))
                    return;

                if (!c->sDCBlockBank_ext.init(FILTER_CHAINS_MAX))
                    return;

                if (!c->sOversampler_x.init())
                    return;

                if (!c->sOversampler_y.init())
                    return;

                if (!c->sOversampler_ext.init())
                    return;

                if (!c->sPreTrgDelay.init(PRE_TRG_MAX_SIZE))
                    return;

                // Settings for the Sweep Generator
                c->sSweepGenerator.init();
                c->sSweepGenerator.set_phase_accumulator_bits(SWEEP_GEN_N_BITS);
                c->sSweepGenerator.set_phase(0.0f);
                c->sSweepGenerator.update_settings();

                c->vTemp                = advance_ptr<float>(ptr, BUF_LIM_SIZE);
                c->vData_x              = advance_ptr<float>(ptr, BUF_LIM_SIZE);
                c->vData_y              = advance_ptr<float>(ptr, BUF_LIM_SIZE);
                c->vData_ext            = advance_ptr<float>(ptr, BUF_LIM_SIZE);
                c->vData_y_delay        = advance_ptr<float>(ptr, BUF_LIM_SIZE);
                c->vDisplay_x           = advance_ptr<float>(ptr, BUF_LIM_SIZE);
                c->vDisplay_y           = advance_ptr<float>(ptr, BUF_LIM_SIZE);
                c->vDisplay_s           = advance_ptr<float>(ptr, BUF_LIM_SIZE);
                c->vIDisplay_x          = advance_ptr<float>(ptr, BUF_LIM_SIZE);
                c->vIDisplay_y          = advance_ptr<float>(ptr, BUF_LIM_SIZE);

                c->nIDisplay            = 0;

                c->nDataHead            = 0;
                c->nDisplayHead         = 0;
                c->nSamplesCounter      = 0;
                c->bClearStream         = false;

                c->nPreTrigger          = 0;
                c->nSweepSize           = 0;

                c->fVerStreamScale      = 0.0f;
                c->fVerStreamOffset     = 0.0f;

                c->bAutoSweep           = true;
                c->nAutoSweepLimit      = 0;
                c->nAutoSweepCounter    = 0;

                c->enState              = CH_STATE_LISTENING;

                c->vIn_x                = NULL;
                c->vIn_y                = NULL;
                c->vIn_ext              = NULL;

                c->vOut_x               = NULL;
                c->vOut_y               = NULL;

                c->pIn_x                = NULL;
                c->pIn_y                = NULL;
                c->pIn_ext              = NULL;

                c->pOut_x               = NULL;
                c->pOut_y               = NULL;

                c->pOvsMode             = NULL;
                c->pScpMode             = NULL;
                c->pCoupling_x          = NULL;
                c->pCoupling_y          = NULL;
                c->pCoupling_ext        = NULL;

                c->pSweepType           = NULL;
                c->pTimeDiv             = NULL;
                c->pHorPos              = NULL;

                c->pVerDiv              = NULL;
                c->pVerPos              = NULL;

                c->pTrgHys              = NULL;
                c->pTrgLev              = NULL;
                c->pTrgHold             = NULL;
                c->pTrgMode             = NULL;
                c->pTrgType             = NULL;
                c->pTrgInput            = NULL;
                c->pTrgReset            = NULL;

                c->pGlobalSwitch        = NULL;
                c->pFreezeSwitch        = NULL;
                c->pSoloSwitch          = NULL;
                c->pMuteSwitch          = NULL;

                c->pStream              = NULL;
            }

            lsp_assert(ptr <= &save[samples]);

            // Bind ports
            size_t port_id = 0;

            lsp_trace("Binding audio ports");

            for (size_t ch = 0; ch < nChannels; ++ch)
            {
                channel_t *c        = &vChannels[ch];

                BIND_PORT(c->pIn_x);
                BIND_PORT(c->pIn_y);
                BIND_PORT(c->pIn_ext);
                BIND_PORT(c->pOut_x);
                BIND_PORT(c->pOut_y);
            }

            // Common settings
            lsp_trace("Binding common ports");

            BIND_PORT(pStrobeHistSize);
            BIND_PORT(pXYRecordTime);
            SKIP_PORT("maxdots parameter");
            BIND_PORT(pFreeze);

            // Global ports only exists on multi-channel versions. Skip for 1X plugin.
            lsp_trace("Binding global control ports");
            if (nChannels > 1)
            {
                // Channel selector only exists on multi-channel versions
                BIND_PORT(pChannelSelector);
                BIND_PORT(pOvsMode);
                BIND_PORT(pScpMode);
                BIND_PORT(pCoupling_x);
                BIND_PORT(pCoupling_y);
                BIND_PORT(pCoupling_ext);
                BIND_PORT(pSweepType);
                BIND_PORT(pTimeDiv);
                BIND_PORT(pHorDiv);
                BIND_PORT(pHorPos);
                BIND_PORT(pVerDiv);
                BIND_PORT(pVerPos);
                BIND_PORT(pTrgHys);
                BIND_PORT(pTrgLev);
                BIND_PORT(pTrgHold);
                BIND_PORT(pTrgMode);
                BIND_PORT(pTrgType);
                BIND_PORT(pTrgInput);
                BIND_PORT(pTrgReset);
            }

            lsp_trace("Binding channel control ports");
            for (size_t ch = 0; ch < nChannels; ++ch)
            {
                channel_t *c        = &vChannels[ch];

                BIND_PORT(c->pOvsMode);
                BIND_PORT(c->pScpMode);
                BIND_PORT(c->pCoupling_x);
                BIND_PORT(c->pCoupling_y);
                BIND_PORT(c->pCoupling_ext);
                BIND_PORT(c->pSweepType);
                BIND_PORT(c->pTimeDiv);
                BIND_PORT(c->pHorDiv);
                BIND_PORT(c->pHorPos);
                BIND_PORT(c->pVerDiv);
                BIND_PORT(c->pVerPos);
                BIND_PORT(c->pTrgHys);
                BIND_PORT(c->pTrgLev);
                BIND_PORT(c->pTrgHold);
                BIND_PORT(c->pTrgMode);
                BIND_PORT(c->pTrgType);
                BIND_PORT(c->pTrgInput);
                BIND_PORT(c->pTrgReset);
            }

            lsp_trace("Binding channel switches ports");
            if (nChannels > 1)
            {
                for (size_t ch = 0; ch < nChannels; ++ch)
                {
                    channel_t *c        = &vChannels[ch];

                    BIND_PORT(c->pGlobalSwitch);
                    BIND_PORT(c->pFreezeSwitch);
                    BIND_PORT(c->pSoloSwitch);
                    BIND_PORT(c->pMuteSwitch);
                }
            }

            lsp_trace("Binding channel visual outputs ports");
            for (size_t ch = 0; ch < nChannels; ++ch)
            {
                channel_t *c        = &vChannels[ch];
                BIND_PORT(c->pStream);
            }
        }

        dspu::over_mode_t oscilloscope::get_oversampler_mode(size_t portValue)
        {
            switch (portValue)
            {
                case meta::oscilloscope_metadata::OSC_OVS_NONE:
                    return dspu::OM_NONE;
                case meta::oscilloscope_metadata::OSC_OVS_2X:
                    return dspu::OM_LANCZOS_2X24BIT;
                case meta::oscilloscope_metadata::OSC_OVS_3X:
                    return dspu::OM_LANCZOS_3X24BIT;
                case meta::oscilloscope_metadata::OSC_OVS_4X:
                    return dspu::OM_LANCZOS_4X24BIT;
                case meta::oscilloscope_metadata::OSC_OVS_6X:
                    return dspu::OM_LANCZOS_6X24BIT;
                case meta::oscilloscope_metadata::OSC_OVS_8X:
                default:
                    return dspu::OM_LANCZOS_8X24BIT;
            }
        }

        oscilloscope::ch_mode_t oscilloscope::get_scope_mode(size_t portValue)
        {
            switch (portValue)
            {
                case meta::oscilloscope_metadata::MODE_XY:
                    return CH_MODE_XY;
                case meta::oscilloscope_metadata::MODE_TRIGGERED:
                    return CH_MODE_TRIGGERED;
                case meta::oscilloscope_metadata::MODE_GONIOMETER:
                    return CH_MODE_GONIOMETER;
                default:
                    return CH_MODE_DFL;
            }
        }

        oscilloscope::ch_sweep_type_t oscilloscope::get_sweep_type(size_t portValue)
        {
            switch (portValue)
            {
                case meta::oscilloscope_metadata::SWEEP_TYPE_SAWTOOTH:
                    return CH_SWEEP_TYPE_SAWTOOTH;
                case meta::oscilloscope_metadata::SWEEP_TYPE_TRIANGULAR:
                    return CH_SWEEP_TYPE_TRIANGULAR;
                case meta::oscilloscope_metadata::SWEEP_TYPE_SINE:
                    return CH_SWEEP_TYPE_SINE;
                default:
                    return CH_SWEEP_TYPE_DFL;
            }
        }

        oscilloscope::ch_trg_input_t oscilloscope::get_trigger_input(size_t portValue)
        {
            switch (portValue)
            {
                case meta::oscilloscope_metadata::TRIGGER_INPUT_Y:
                    return CH_TRG_INPUT_Y;
                case meta::oscilloscope_metadata::TRIGGER_INPUT_EXT:
                    return CH_TRG_INPUT_EXT;
                default:
                    return CH_TRG_INPUT_DFL;
            }
        }

        oscilloscope::ch_coupling_t oscilloscope::get_coupling_type(size_t portValue)
        {
            switch (portValue)
            {
                case meta::oscilloscope_metadata::COUPLING_AC:
                    return CH_COUPLING_AC;
                case meta::oscilloscope_metadata::COUPLING_DC:
                    return CH_COUPLING_DC;
                default:
                    return CH_COUPLING_DFL;

            }
        }

        dspu::trg_mode_t oscilloscope::get_trigger_mode(size_t portValue)
        {
            switch (portValue)
            {
                case meta::oscilloscope_metadata::TRIGGER_MODE_SINGLE:
                    return dspu::TRG_MODE_SINGLE;
                case  meta::oscilloscope_metadata::TRIGGER_MODE_MANUAL:
                    return dspu::TRG_MODE_MANUAL;
                case meta::oscilloscope_metadata::TRIGGER_MODE_REPEAT:
                    return dspu::TRG_MODE_REPEAT;
                default:
                    return dspu::TRG_MODE_REPEAT;
            }
        }

        dspu::trg_type_t oscilloscope::get_trigger_type(size_t portValue)
        {
            switch (portValue)
            {
                case meta::oscilloscope_metadata::TRIGGER_TYPE_NONE:
                    return dspu::TRG_TYPE_NONE;
                case meta::oscilloscope_metadata::TRIGGER_TYPE_SIMPLE_RISING_EDGE:
                    return dspu::TRG_TYPE_SIMPLE_RISING_EDGE;
                case meta::oscilloscope_metadata::TRIGGER_TYPE_SIMPE_FALLING_EDGE:
                    return dspu::TRG_TYPE_SIMPLE_FALLING_EDGE;
                case meta::oscilloscope_metadata::TRIGGER_TYPE_ADVANCED_RISING_EDGE:
                    return dspu::TRG_TYPE_ADVANCED_RISING_EDGE;
                case meta::oscilloscope_metadata::TRIGGER_TYPE_ADVANCED_FALLING_EDGE:
                    return dspu::TRG_TYPE_ADVANCED_FALLING_EDGE;
                default:
                    return dspu::TRG_TYPE_NONE;
            }
        }

        void oscilloscope::update_dc_block_filter(dspu::FilterBank &rFilterBank)
        {
            /* Filter Transfer Function:
             *
             *          g - g z^-1
             * H(z) = ----------------
             *          1 - a * z^-1
             *
             * With g = sDCBlockParams.fGain, a = sACBlockParams.fAlpha
             */

            rFilterBank.begin();

            dsp::biquad_x1_t *f = rFilterBank.add_chain();
            if (f == NULL)
                return;

            f->b0   = sDCBlockParams.fGain;
            f->b1   = -sDCBlockParams.fGain;
            f->b2   = 0.0f;
            f->a1   = sDCBlockParams.fAlpha;
            f->a2   = 0.0f;
            f->p0   = 0.0f;
            f->p1   = 0.0f;
            f->p2   = 0.0f;

            rFilterBank.end(true);
        }

        void oscilloscope::reconfigure_dc_block_filters()
        {
            double omega = 2.0 * M_PI * DC_BLOCK_CUTOFF_HZ / fSampleRate; // Normalised frequency

            double c = cos(omega);
            double g = 1.9952623149688795; // This is 10^(3/10), used to calculate the parameter alpha so that it is exactly associated to the cutoff frequency (-3 dB).
            double r = sqrt(c*c - 1.0 - 2.0 * g * c + 2.0 * g);

            double alpha1 = c + r;
            double alpha2 = c - r;

            if ((alpha1 >= 0.0) && (alpha1 < 1.0))
                sDCBlockParams.fAlpha = alpha1;
            else if ((alpha2 >= 0.0) && (alpha2 < 1.0))
                sDCBlockParams.fAlpha = alpha2;
            else
                sDCBlockParams.fAlpha = DC_BLOCK_DFL_ALPHA;

            sDCBlockParams.fGain = 0.5f * (1.0f + sDCBlockParams.fAlpha);

            for (size_t ch = 0; ch < nChannels; ++ch)
            {
                channel_t *c = &vChannels[ch];

                update_dc_block_filter(c->sDCBlockBank_x);
                update_dc_block_filter(c->sDCBlockBank_y);
                update_dc_block_filter(c->sDCBlockBank_ext);
            }
        }

        void oscilloscope::do_sweep_step(channel_t *c, float strobe_value)
        {
            c->sSweepGenerator.process_overwrite(&c->vDisplay_x[c->nDisplayHead], 1);
            c->vDisplay_y[c->nDisplayHead] = c->vData_y_delay[c->nDataHead];
            c->vDisplay_s[c->nDisplayHead] = strobe_value;
            ++c->nDataHead;
            ++c->nDisplayHead;
        }

        float *oscilloscope::select_trigger_input(float *extPtr, float* yPtr, ch_trg_input_t input)
        {
            switch (input)
            {
                case CH_TRG_INPUT_EXT:
                    return extPtr;

                case CH_TRG_INPUT_Y:
                default:
                    return yPtr;
            }
        }

        void oscilloscope::set_oversampler(dspu::Oversampler &over, dspu::over_mode_t mode)
        {
            over.set_mode(mode);
            if (over.modified())
                over.update_settings();
        }

        void oscilloscope::set_sweep_generator(channel_t *c)
        {
            c->sSweepGenerator.set_sample_rate(c->nOverSampleRate);
            c->sSweepGenerator.set_frequency(c->nOverSampleRate / c->nSweepSize);

            switch (c->enSweepType)
            {
                case CH_SWEEP_TYPE_TRIANGULAR:
                {
                    c->sSweepGenerator.set_function(dspu::FG_SAWTOOTH);
                    c->sSweepGenerator.set_dc_reference(dspu::DC_WAVEDC);
                    c->sSweepGenerator.set_amplitude(SWEEP_GEN_PEAK);
                    c->sSweepGenerator.set_dc_offset(0.0f);
                    c->sSweepGenerator.set_width(0.5f);
                }
                break;

                case CH_SWEEP_TYPE_SINE:
                {
                    c->sSweepGenerator.set_function(dspu::FG_SINE);
                    c->sSweepGenerator.set_dc_reference(dspu::DC_WAVEDC);
                    c->sSweepGenerator.set_amplitude(SWEEP_GEN_PEAK);
                    c->sSweepGenerator.set_dc_offset(0.0f);
                }
                break;

                case CH_SWEEP_TYPE_SAWTOOTH:
                default:
                {
                    c->sSweepGenerator.set_function(dspu::FG_SAWTOOTH);
                    c->sSweepGenerator.set_dc_reference(dspu::DC_WAVEDC);
                    c->sSweepGenerator.set_amplitude(SWEEP_GEN_PEAK);
                    c->sSweepGenerator.set_dc_offset(0.0f);
                    c->sSweepGenerator.set_width(1.0f);
                }
                break;
            }

            c->sSweepGenerator.update_settings();
        }

        void oscilloscope::configure_oversamplers(channel_t *c, dspu::over_mode_t mode)
        {
            c->enOverMode = mode;

            set_oversampler(c->sOversampler_x, c->enOverMode);
            set_oversampler(c->sOversampler_y, c->enOverMode);
            set_oversampler(c->sOversampler_ext, c->enOverMode);

            // All are set the same way, use any to get these variables
            c->nOversampling    = c->sOversampler_x.get_oversampling();
            c->nOverSampleRate  = c->nOversampling * fSampleRate;
        }

        void oscilloscope::init_state_stage(channel_t *c)
        {
            c->nUpdate = 0;

            c->sStateStage.nPV_pScpMode = meta::oscilloscope_metadata::MODE_DFL;
            c->nUpdate |= UPD_SCPMODE;

            c->sStateStage.nPV_pCoupling_x = meta::oscilloscope_metadata::COUPLING_DFL;
            c->nUpdate |= UPD_ACBLOCK_X;

            c->sStateStage.nPV_pCoupling_y = meta::oscilloscope_metadata::COUPLING_DFL;
            c->nUpdate |= UPD_ACBLOCK_Y;

            c->sStateStage.nPV_pCoupling_ext = meta::oscilloscope_metadata::COUPLING_DFL;
            c->nUpdate |= UPD_ACBLOCK_EXT;

            c->sStateStage.nPV_pOvsMode = meta::oscilloscope_metadata::OSC_OVS_DFL;
            c->nUpdate |= UPD_OVERSAMPLER_X;
            c->nUpdate |= UPD_OVERSAMPLER_Y;
            c->nUpdate |= UPD_OVERSAMPLER_EXT;

            c->sStateStage.nPV_pTrgInput = meta::oscilloscope_metadata::TRIGGER_INPUT_DFL;
            c->nUpdate |= UPD_TRIGGER_INPUT;

            c->sStateStage.fPV_pVerDiv = meta::oscilloscope_metadata::VERTICAL_DIVISION_DFL;
            c->sStateStage.fPV_pVerPos = meta::oscilloscope_metadata::VERTICAL_POSITION_DFL;
            c->nUpdate |= UPD_TRIGGER;

            c->sStateStage.fPV_pTrgHys = meta::oscilloscope_metadata::TRIGGER_HYSTERESIS_DFL;
            c->nUpdate |= UPD_TRIGGER;

            c->sStateStage.fPV_pTrgLevel = meta::oscilloscope_metadata::TRIGGER_LEVEL_DFL;
            c->nUpdate |= UPD_TRIGGER;

            c->sStateStage.nPV_pTrgMode = meta::oscilloscope_metadata::TRIGGER_MODE_DFL;
            c->nUpdate |= UPD_TRIGGER;

            c->sStateStage.fPV_pTrgHold = meta::oscilloscope_metadata::TRIGGER_HOLD_TIME_DFL;
            c->nUpdate |= UPD_TRIGGER_HOLD;

            c->sStateStage.nPV_pTrgType = meta::oscilloscope_metadata::TRIGGER_TYPE_DFL;
            c->nUpdate |= UPD_TRIGGER;

            c->sStateStage.fPV_pTimeDiv = meta::oscilloscope_metadata::TIME_DIVISION_DFL;
            c->nUpdate |= UPD_SWEEP_GENERATOR;

            c->sStateStage.fPV_pHorDiv = meta::oscilloscope_metadata::HORIZONTAL_DIVISION_DFL;
            c->sStateStage.fPV_pHorPos = meta::oscilloscope_metadata::TIME_POSITION_DFL;
            c->nUpdate |= UPD_SWEEP_GENERATOR;
            c->nUpdate |= UPD_PRETRG_DELAY;

            c->sStateStage.nPV_pSweepType = meta::oscilloscope_metadata::SWEEP_TYPE_DFL;
            c->nUpdate |= UPD_SWEEP_GENERATOR;

            c->sStateStage.fPV_pXYRecordTime = meta::oscilloscope_metadata::XY_RECORD_TIME_DFL;
            c->nUpdate |= UPD_XY_RECORD_TIME;

            c->nUpdate |= UPD_VER_SCALES;
            c->nUpdate |= UPD_HOR_SCALES;

            // By default, this must be false.
            c->bUseGlobal   = false;
            c->bFreeze      = false;
            c->bVisible     = false;
        }

        void oscilloscope::commit_staged_state_change(channel_t *c)
        {
            if (c->nUpdate == 0)
                return;

            if (c->nUpdate & UPD_SCPMODE)
            {
                c->enMode           = get_scope_mode(c->sStateStage.nPV_pScpMode);
                c->nDisplayHead     = 0;    // Reset the display head
            }

            if (c->nUpdate & UPD_ACBLOCK_X)
                c->enCoupling_x = get_coupling_type(c->sStateStage.nPV_pCoupling_x);

            if (c->nUpdate & UPD_ACBLOCK_Y)
                c->enCoupling_y = get_coupling_type(c->sStateStage.nPV_pCoupling_y);

            if (c->nUpdate & UPD_ACBLOCK_EXT)
                c->enCoupling_ext = get_coupling_type(c->sStateStage.nPV_pCoupling_ext);

            if (c->nUpdate & (UPD_OVERSAMPLER_X | UPD_OVERSAMPLER_Y | UPD_OVERSAMPLER_EXT))
                configure_oversamplers(c, get_oversampler_mode(c->sStateStage.nPV_pOvsMode));

            if (c->nUpdate & UPD_XY_RECORD_TIME)
            {
                c->nXYRecordSize = dspu::millis_to_samples(c->nOverSampleRate, c->sStateStage.fPV_pXYRecordTime);
                c->nXYRecordSize = (c->nXYRecordSize < BUF_LIM_SIZE) ? c->nXYRecordSize  : BUF_LIM_SIZE;
            }

            // UPD_SWEEP_GENERATOR handling is split because if also UPD_PRETRG_DELAY needs to be handled them the correct order of operations is as follows.
            if (c->nUpdate & UPD_SWEEP_GENERATOR)
            {
                c->nSweepSize = STREAM_N_HOR_DIV * dspu::millis_to_samples(c->nOverSampleRate, c->sStateStage.fPV_pTimeDiv);
                c->nSweepSize = (c->nSweepSize < BUF_LIM_SIZE) ? c->nSweepSize  : BUF_LIM_SIZE;
            }

            if (c->nUpdate & UPD_PRETRG_DELAY)
            {
                c->nPreTrigger = 0.5f * (0.01f * c->sStateStage.fPV_pHorPos  + 1) * (c->nSweepSize - 1);
                c->nPreTrigger = (c->nPreTrigger < PRE_TRG_MAX_SIZE) ? c->nPreTrigger : PRE_TRG_MAX_SIZE;
                c->sPreTrgDelay.set_delay(c->nPreTrigger);
                c->sPreTrgDelay.clear();
            }

            if (c->nUpdate & UPD_SWEEP_GENERATOR)
            {
                c->enSweepType = get_sweep_type(c->sStateStage.nPV_pSweepType);
                set_sweep_generator(c);

                // Since the seep period has changed, we need to revert state to LISTENING.
                c->enState = CH_STATE_LISTENING;
            }

            if (c->nUpdate & UPD_TRIGGER_INPUT)
                c->enTrgInput = get_trigger_input(c->sStateStage.nPV_pTrgInput);

            if (c->nUpdate & UPD_TRIGGER_HOLD)
            {
                size_t minHold = c->nSweepSize;
                size_t trgHold = dspu::seconds_to_samples(c->nOverSampleRate, c->sStateStage.fPV_pTrgHold);
                trgHold = trgHold > minHold ? trgHold : minHold;
                c->sTrigger.set_trigger_hold_samples(trgHold);

                c->nAutoSweepLimit      = dspu::seconds_to_samples(c->nOverSampleRate, AUTO_SWEEP_TIME);
                c->nAutoSweepLimit      = (c->nAutoSweepLimit < trgHold) ? trgHold: c->nAutoSweepLimit;
                c->nAutoSweepCounter    = 0;
            }

            if (c->nUpdate & UPD_HOR_SCALES)
            {
                c->fHorStreamScale      = (STREAM_MAX_X - STREAM_MIN_X) / (STREAM_N_HOR_DIV * c->sStateStage.fPV_pHorDiv);
                c->fHorStreamOffset     = 0.5f * (STREAM_MAX_X - STREAM_MIN_X) * (0.01f * c->sStateStage.fPV_pHorPos + 1.0f) + STREAM_MIN_X;
            }

            if (c->nUpdate & UPD_VER_SCALES)
            {
                c->fVerStreamScale      = (STREAM_MAX_Y - STREAM_MIN_Y) / (STREAM_N_VER_DIV * c->sStateStage.fPV_pVerDiv);
                c->fVerStreamOffset     = 0.5f * (STREAM_MAX_Y - STREAM_MIN_Y) * (0.01f * c->sStateStage.fPV_pVerPos + 1.0f) + STREAM_MIN_Y;
            }

            if (c->nUpdate & UPD_TRIGGER)
            {
                dspu::trg_mode_t trgMode= get_trigger_mode(c->sStateStage.nPV_pTrgMode);

                c->bAutoSweep           = !((trgMode == dspu::TRG_MODE_SINGLE) || (trgMode == dspu::TRG_MODE_MANUAL));
                c->sTrigger.set_trigger_mode(trgMode);
                c->sTrigger.set_trigger_hysteresis(0.01f * c->sStateStage.fPV_pTrgHys * STREAM_N_VER_DIV * c->sStateStage.fPV_pVerDiv);
                c->sTrigger.set_trigger_type(get_trigger_type(c->sStateStage.nPV_pTrgType));
                c->sTrigger.set_trigger_threshold(0.5f * STREAM_N_VER_DIV * c->sStateStage.fPV_pVerDiv * 0.01f * c->sStateStage.fPV_pTrgLevel);
                c->sTrigger.update_settings();
            }

            if (c->nUpdate & UPD_TRGGER_RESET)
            {
                c->sTrigger.reset_single_trigger();
                c->sTrigger.activate_manual_trigger();
            }

            c->bClearStream = true;

            // Clear the update flag
            c->nUpdate = 0;
        }

        bool oscilloscope::graph_stream(channel_t * c)
        {
            // Remember size and reset head
            size_t query_size   = c->nDisplayHead;
            c->nDisplayHead     = 0;

            // Check that stream is present
            plug::stream_t *stream = c->pStream->buffer<plug::stream_t>();
            if ((stream == NULL) || (c->bFreeze))
                return false;

            if (c->bClearStream)
            {
                stream->clear();
                c->bClearStream = false;
            }

            // Transform XY -> MS for goniomteter mode
            if (c->enMode == CH_MODE_GONIOMETER)
                dsp::lr_to_ms(c->vDisplay_y, c->vDisplay_x, c->vDisplay_y, c->vDisplay_x, query_size);

            // In-place decimation:
            size_t j = 0;

            for (size_t i = 1; i < query_size; ++i)
            {
                float dx    = c->vDisplay_x[i] - c->vDisplay_x[j];
                float dy    = c->vDisplay_y[i] - c->vDisplay_y[j];
                float s     = dx*dx + dy*dy;

                if (s < DECIM_PRECISION) // Skip point
                {
                    c->vDisplay_s[j] = lsp_max(c->vDisplay_s[i], c->vDisplay_s[j]); // Keep the strobe signal
                    continue;
                }

                // Add point to decimated array
                ++j;
                c->vDisplay_x[j] = c->vDisplay_x[i];
                c->vDisplay_y[j] = c->vDisplay_y[i];
            }

            // Detect occasional jumps
            size_t to_submit = j + 1; // Total number of decimated samples.

            // Apply scaling and offset:
            dsp::mul_k2(c->vDisplay_y, c->fVerStreamScale, to_submit);
            dsp::add_k2(c->vDisplay_y, c->fVerStreamOffset, to_submit);

            // x is to be scaled and offset only in XY mode
            if ((c->enMode == CH_MODE_XY) || (c->enMode == CH_MODE_GONIOMETER))
            {
                dsp::mul_k2(c->vDisplay_x, c->fHorStreamScale, to_submit);
                dsp::add_k2(c->vDisplay_x, c->fHorStreamOffset, to_submit);
            }

    //    #ifdef LSP_TRACE
    //        for (size_t i=1; i < to_submit; ++i)
    //        {
    //            float dx    = c->vDisplay_x[i] - c->vDisplay_x[i-1];
    //            float dy    = c->vDisplay_y[i] - c->vDisplay_y[i-1];
    //            float s     = dx*dx + dy*dy;
    //            if ((s >= 0.125f) && (c->vDisplay_s[i] <= 0.5f))
    //            {
    //                lsp_trace("debug");
    //            }
    //        }
    //    #endif

            // Submit data for plotting (emit the figure data with fixed-size frames):
            for (size_t i = 0; i < to_submit; )  // nSweepSize can be as big as BUF_LIM_SIZE !!!
            {
                size_t count = stream->add_frame(to_submit - i);     // Add a frame
                stream->write_frame(0, &c->vDisplay_x[i], 0, count); // X'es
                stream->write_frame(1, &c->vDisplay_y[i], 0, count); // Y's
                stream->write_frame(2, &c->vDisplay_s[i], 0, count); // Strobe signal
                stream->commit_frame();                              // Commit the frame

                // Move the index in the source buffer
                i += count;
            }

            // Is there data to submit to inline display?
            if (to_submit > 0)
            {
                // Compute the start point to submit data
                j   = 0;

                // In-place decimation:
                for (size_t i = j+1; i < to_submit; ++i)
                {
                    float dx    = c->vDisplay_x[i] - c->vDisplay_x[j];
                    float dy    = c->vDisplay_y[i] - c->vDisplay_y[j];
                    float s     = dx*dx + dy*dy;

                    if (s < IDISPLAY_DECIM) // Skip point
                        continue;

                    // Add point to decimated array
                    ++j;
                    c->vDisplay_x[j] = c->vDisplay_x[i];
                    c->vDisplay_y[j] = c->vDisplay_y[i];
                }

                // Copy display data to inline display buffer.
                c->nIDisplay = j + 1;
                dsp::copy(c->vIDisplay_x, c->vDisplay_x, c->nIDisplay);
                dsp::copy(c->vIDisplay_y, c->vDisplay_y, c->nIDisplay);
            }

            return true;
        }

        void oscilloscope::update_settings()
        {
            float xy_rectime    = pXYRecordTime->value();
            bool g_freeze       = pFreeze->value() >= 0.5f;
            bool has_solo       = false;

            for (size_t ch = 0; ch < nChannels; ++ch)
            {
                channel_t *c    = &vChannels[ch];
                bool solo       = (c->pSoloSwitch != NULL) ? c->pSoloSwitch->value() >= 0.5f : false;
                if (solo)
                    has_solo        = true;
            }

            for (size_t ch = 0; ch < nChannels; ++ch)
            {
                channel_t *c = &vChannels[ch];

                // Global controls only actually exist for mult-channel plugins. Do not use for 1X.
                if (nChannels > 1)
                    c->bUseGlobal = c->pGlobalSwitch->value() >= 0.5f;

                bool solo       = (c->pSoloSwitch != NULL) ? c->pSoloSwitch->value() >= 0.5f : false;
                bool mute       = (c->pMuteSwitch != NULL) ? c->pMuteSwitch->value() >= 0.5f : false;
                c->bVisible     = (has_solo) ? solo : !mute;

                c->bFreeze      = g_freeze;
                if ((!c->bFreeze) && (nChannels > 1))
                    c->bFreeze      = c->pFreezeSwitch->value() >= 0.5f;

                if (xy_rectime != c->sStateStage.fPV_pXYRecordTime)
                {
                    c->sStateStage.fPV_pXYRecordTime = xy_rectime;
                    c->nUpdate |= UPD_XY_RECORD_TIME;
                }

                size_t scpmode = (c->bUseGlobal) ? pScpMode->value() : c->pScpMode->value();
                if (scpmode != c->sStateStage.nPV_pScpMode)
                {
                    c->sStateStage.nPV_pScpMode = scpmode;
                    c->nUpdate |= UPD_SCPMODE;
                }

                size_t coupling_x = (c->bUseGlobal) ? pCoupling_x->value() : c->pCoupling_x->value();
                if (coupling_x != c->sStateStage.nPV_pCoupling_x)
                {
                    c->sStateStage.nPV_pCoupling_x = coupling_x;
                    c->nUpdate |= UPD_ACBLOCK_X;
                }

                size_t coupling_y = (c->bUseGlobal) ? pCoupling_y->value() : c->pCoupling_y->value();
                if (coupling_y != c->sStateStage.nPV_pCoupling_y)
                {
                    c->sStateStage.nPV_pCoupling_y = coupling_y;
                    c->nUpdate |= UPD_ACBLOCK_Y;
                }

                size_t coupling_ext = (c->bUseGlobal) ? pCoupling_ext->value() : c->pCoupling_ext->value();
                if (coupling_ext != c->sStateStage.nPV_pCoupling_ext)
                {
                    c->sStateStage.nPV_pCoupling_ext = coupling_ext;
                    c->nUpdate |= UPD_ACBLOCK_EXT;
                }

                size_t overmode = (c->bUseGlobal) ? pOvsMode->value() : c->pOvsMode->value();
                if (overmode != c->sStateStage.nPV_pOvsMode)
                {
                    c->sStateStage.nPV_pOvsMode = overmode;
                    c->nUpdate |= UPD_OVERSAMPLER_X | UPD_OVERSAMPLER_Y | UPD_OVERSAMPLER_EXT |
                                  UPD_PRETRG_DELAY | UPD_SWEEP_GENERATOR | UPD_TRIGGER_HOLD |
                                  UPD_XY_RECORD_TIME;
                }

                size_t trginput = (c->bUseGlobal) ? pTrgInput->value() : c->pTrgInput->value();
                if (trginput != c->sStateStage.nPV_pTrgInput)
                {
                    c->sStateStage.nPV_pTrgInput = trginput;
                    c->nUpdate |= UPD_TRIGGER_INPUT;
                }

                float verDiv = (c->bUseGlobal) ? pVerDiv->value() : c->pVerDiv->value();
                float verPos = (c->bUseGlobal) ? pVerPos->value() : c->pVerPos->value();
                if ((verDiv != c->sStateStage.fPV_pVerDiv) || (verPos != c->sStateStage.fPV_pVerPos))
                {
                    c->sStateStage.fPV_pVerDiv = verDiv;
                    c->sStateStage.fPV_pVerPos = verPos;
                    c->nUpdate |= UPD_VER_SCALES | UPD_TRIGGER;
                }

                float trgHys = (c->bUseGlobal) ? pTrgHys->value() : c->pTrgHys->value();
                if (trgHys != c->sStateStage.fPV_pTrgHys)
                {
                    c->sStateStage.fPV_pTrgHys = trgHys;
                    c->nUpdate |= UPD_TRIGGER;
                }

                float trgLevel = (c->bUseGlobal) ? pTrgLev->value() : c->pTrgLev->value();
                if (trgLevel != c->sStateStage.fPV_pTrgLevel)
                {
                    c->sStateStage.fPV_pTrgLevel = trgLevel;
                    c->nUpdate |= UPD_TRIGGER;
                }

                size_t trgmode = (c->bUseGlobal) ? pTrgMode->value() : c->pTrgMode->value();
                if (trgmode != c->sStateStage.nPV_pTrgMode)
                {
                    c->sStateStage.nPV_pTrgMode = trgmode;
                    c->nUpdate |= UPD_TRIGGER;
                }

                float trghold = (c->bUseGlobal) ? pTrgHold->value() : c->pTrgHold->value();
                if (trghold != c->sStateStage.fPV_pTrgHold)
                {
                    c->sStateStage.fPV_pTrgHold = trghold;
                    c->nUpdate |= UPD_TRIGGER_HOLD;
                }

                size_t trgtype = (c->bUseGlobal) ? pTrgType->value() : c->pTrgType->value();
                if (trgtype != c->sStateStage.nPV_pTrgType)
                {
                    c->sStateStage.nPV_pTrgType = trgtype;
                    c->nUpdate |= UPD_TRIGGER;
                }

                float trg_reset = (c->bUseGlobal) ? pTrgReset->value() : c->pTrgReset->value();
                if (trg_reset >= 0.5f)
                    c->nUpdate |= UPD_TRGGER_RESET;

                float timeDiv = (c->bUseGlobal) ? pTimeDiv->value() : c->pTimeDiv->value();
                if (timeDiv != c->sStateStage.fPV_pTimeDiv)
                {
                    c->sStateStage.fPV_pTimeDiv = timeDiv;
                    c->nUpdate |= UPD_PRETRG_DELAY | UPD_SWEEP_GENERATOR | UPD_TRIGGER_HOLD;
                }

                float horDiv = (c->bUseGlobal) ? pHorDiv->value() : c->pHorDiv->value();
                if (horDiv != c->sStateStage.fPV_pHorDiv)
                {
                    c->sStateStage.fPV_pHorDiv = horDiv;
                    c->nUpdate |= UPD_HOR_SCALES;
                }

                float horPos = (c->bUseGlobal) ? pHorPos->value() : c->pHorPos->value();
                if (horPos != c->sStateStage.fPV_pHorPos)
                {
                    c->sStateStage.fPV_pHorPos = horPos;
                    c->nUpdate |= UPD_HOR_SCALES | UPD_PRETRG_DELAY | UPD_SWEEP_GENERATOR;
                }

                size_t sweeptype = (c->bUseGlobal) ? pSweepType->value() : c->pSweepType->value();
                if (sweeptype != c->sStateStage.nPV_pSweepType)
                {
                    c->sStateStage.nPV_pSweepType = sweeptype;
                    c->nUpdate |= UPD_SWEEP_GENERATOR;
                }
            }
        }

        void oscilloscope::update_sample_rate(long sr)
        {
            reconfigure_dc_block_filters();

            for (size_t ch = 0; ch < nChannels; ++ch)
            {
                channel_t *c = &vChannels[ch];

                c->sOversampler_x.set_sample_rate(sr);
                c->sOversampler_x.update_settings();

                c->sOversampler_y.set_sample_rate(sr);
                c->sOversampler_y.update_settings();

                c->sOversampler_ext.set_sample_rate(sr);
                c->sOversampler_ext.update_settings();

                c->nOverSampleRate = c->nOversampling * sr;

                c->sSweepGenerator.set_sample_rate(sr);
                c->sSweepGenerator.update_settings();
            }
        }

        void oscilloscope::process(size_t samples)
        {
            // Prepare channels
            for (size_t ch = 0; ch < nChannels; ++ch)
            {
                channel_t *c = &vChannels[ch];

                c->vIn_x    = c->pIn_x->buffer<float>();
                c->vIn_y    = c->pIn_y->buffer<float>();
                c->vIn_ext  = c->pIn_ext->buffer<float>();

                c->vOut_x   = c->pOut_x->buffer<float>();
                c->vOut_y   = c->pOut_y->buffer<float>();

                if ((c->vIn_x == NULL) || (c->vIn_y == NULL))
                    return;

                if (c->vIn_ext == NULL)
                    return;

                c->nSamplesCounter = samples;
            }

            // Bypass signal
            for (size_t ch = 0; ch < nChannels; ++ch)
            {
                channel_t *c = &vChannels[ch];

                if (c->vOut_x != NULL)
                    dsp::copy(c->vOut_x, c->vIn_x, samples);
                if (c->vOut_y != NULL)
                    dsp::copy(c->vOut_y, c->vIn_y, samples);
            }

            bool query_draw = false;

            // Process each channel
            for (size_t ch = 0; ch < nChannels; ++ch)
            {
                channel_t *c = &vChannels[ch];

                commit_staged_state_change(c);

                while (c->nSamplesCounter > 0)
                {
                    size_t requested        = c->nOversampling * c->nSamplesCounter;
                    size_t availble         = BUF_LIM_SIZE;
                    size_t to_do_upsample   = (requested < availble) ? requested : availble;
                    size_t to_do            = to_do_upsample / c->nOversampling;

                    switch (c->enMode)
                    {
                        case CH_MODE_XY:
                        case CH_MODE_GONIOMETER:
                        {
                            if (c->enCoupling_x == CH_COUPLING_AC)
                            {
                                c->sDCBlockBank_x.process(c->vTemp, c->vIn_x, to_do);
                                c->sOversampler_x.upsample(c->vData_x, c->vTemp, to_do);
                            }
                            else
                                c->sOversampler_x.upsample(c->vData_x, c->vIn_x, to_do);

                            if (c->enCoupling_y == CH_COUPLING_AC)
                            {
                                c->sDCBlockBank_y.process(c->vTemp, c->vIn_y, to_do);
                                c->sOversampler_y.upsample(c->vData_y, c->vTemp, to_do);
                            }
                            else
                                c->sOversampler_y.upsample(c->vData_y, c->vIn_y, to_do);

                            for (size_t n = 0; n < to_do_upsample; )
                            {
                                ssize_t count = lsp_min(ssize_t(c->nXYRecordSize - c->nDisplayHead), ssize_t(to_do_upsample - n));
                                if (count <= 0)
                                {
                                    // Plot time!
                                    if (graph_stream(c))
                                        query_draw      = true;
                                    continue;
                                }

                                // Move data to intermediate buffers
                                dsp::copy(&c->vDisplay_x[c->nDisplayHead], &c->vData_x[n], count);
                                dsp::copy(&c->vDisplay_y[c->nDisplayHead], &c->vData_y[n], count);
                                dsp::fill_zero(&c->vDisplay_s[c->nDisplayHead], count);
                                if (c->nDisplayHead == 0)
                                    c->vDisplay_s[0]        = 1.0f;

                                // Update pointers
                                c->nDisplayHead    += count;
                                n                  += count;
                            }

                        }
                        break;

                        case CH_MODE_TRIGGERED:
                        {
                            if (c->enCoupling_y == CH_COUPLING_AC)
                            {
                                c->sDCBlockBank_y.process(c->vTemp, c->vIn_y, to_do);
                                c->sOversampler_y.upsample(c->vData_y, c->vTemp, to_do);
                            }
                            else
                                c->sOversampler_y.upsample(c->vData_y, c->vIn_y, to_do);

                            c->sPreTrgDelay.process(c->vData_y_delay, c->vData_y, to_do_upsample);

                            if (c->enCoupling_ext == CH_COUPLING_AC)
                            {
                                c->sDCBlockBank_ext.process(c->vTemp, c->vIn_ext, to_do);
                                c->sOversampler_ext.upsample(c->vData_ext, c->vTemp, to_do);
                            }
                            else
                                c->sOversampler_ext.upsample(c->vData_ext, c->vIn_ext, to_do);

                            c->nDataHead = 0;

                            const float *trg_input = select_trigger_input(c->vData_ext, c->vData_y, c->enTrgInput);

                            for (size_t n = 0; n < to_do_upsample; ++n)
                            {
                                c->sTrigger.single_sample_processor(trg_input[n]);

                                switch (c->enState)
                                {
                                    case CH_STATE_LISTENING:
                                    {
                                        bool sweep = c->sTrigger.get_trigger_state() == dspu::TRG_STATE_FIRED;
                                        if ((!sweep) && (c->bAutoSweep))
                                            sweep = ((c->nAutoSweepCounter++) >= c->nAutoSweepLimit);

                                        // No sweep triggered?
                                        if (!sweep)
                                            break;

                                        c->sSweepGenerator.reset_phase_accumulator();
                                        c->nDataHead            = n;
                                        c->enState              = CH_STATE_SWEEPING;
                                        c->nAutoSweepCounter    = 0;
                                        c->nDisplayHead         = 0;

                                        do_sweep_step(c, 1.0f);

                                        break;
                                    }

                                    case CH_STATE_SWEEPING:
                                        do_sweep_step(c, 0.0f);

                                        if (c->nDisplayHead >= c->nSweepSize)
                                        {
                                            // Plot time!
                                            if (graph_stream(c))
                                                query_draw      = true;
                                            c->enState      = CH_STATE_LISTENING;
                                        }
                                        break;
                                }
                            }
                        }
                        break;
                    }

                    c->vIn_x            += to_do;
                    c->vIn_y            += to_do;
                    c->vIn_ext          += to_do;
                    c->vOut_x           += to_do;
                    c->vOut_y           += to_do;
                    c->nSamplesCounter  -= to_do;
                }
            }

            if ((pWrapper != NULL) && (query_draw))
                pWrapper->query_display_draw();
        }

        void oscilloscope::dump(dspu::IStateDumper *v) const
        {
            plug::Module::dump(v);

            v->begin_object("sDCBlockParams", &sDCBlockParams, sizeof(sDCBlockParams));
            {
                v->write("fAlpha", sDCBlockParams.fAlpha);
                v->write("fGain", sDCBlockParams.fGain);
            }
            v->end_object();

            v->write("nChannels", nChannels);

            v->begin_array("vChannels", vChannels, nChannels);
            for (size_t i = 0; i < nChannels; ++i)
            {
                const channel_t *c = &vChannels[i];

                v->begin_object(c, sizeof(channel_t));
                {
                    v->write("enMode", &c->enMode);
                    v->write("enSweepType", &c->enSweepType);
                    v->write("enTrgInput", &c->enTrgInput);
                    v->write("enCoupling_x", &c->enCoupling_x);
                    v->write("enCoupling_y", &c->enCoupling_y);
                    v->write("enCoupling_ext", &c->enCoupling_ext);

                    v->write_object("sDCBlockBank_x", &c->sDCBlockBank_x);
                    v->write_object("sDCBlockBank_y", &c->sDCBlockBank_y);
                    v->write_object("sDCBlockBank_ext", &c->sDCBlockBank_ext);

                    v->write("enOverMode", &c->enOverMode);
                    v->write("nOversampling", &c->nOversampling);
                    v->write("nOverSampleRate", &c->nOverSampleRate);

                    v->write_object("sOversampler_x", &c->sOversampler_x);
                    v->write_object("sOversampler_y", &c->sOversampler_y);
                    v->write_object("sOversampler_ext", &c->sOversampler_ext);

                    v->write_object("sPreTrgDelay", &c->sPreTrgDelay);

                    v->write_object("sTrigger", &c->sTrigger);

                    v->write_object("sSweepGenerator", &c->sSweepGenerator);

                    v->write("vTemp", &c->vTemp);
                    v->write("vData_x", &c->vData_x);
                    v->write("vData_y", &c->vData_y);
                    v->write("vData_ext", &c->vData_ext);
                    v->write("vData_y_delay", &c->vData_y_delay);
                    v->write("vDisplay_x", &c->vDisplay_x);
                    v->write("vDisplay_y", &c->vDisplay_y);
                    v->write("vDisplay_s", &c->vDisplay_s);

                    v->write("vIDisplay_x", &c->vIDisplay_x);
                    v->write("vIDisplay_y", &c->vIDisplay_y);
                    v->write("nIDisplay", &c->nIDisplay);

                    v->write("nDataHead", &c->nDataHead);
                    v->write("nDisplayHead", &c->nDisplayHead);
                    v->write("nSamplesCounter", &c->nSamplesCounter);
                    v->write("bClearStream", &c->bClearStream);

                    v->write("nPreTrigger", &c->nPreTrigger);
                    v->write("nSweepSize", &c->nSweepSize);

                    v->write("fVerStreamScale", &c->fVerStreamScale);
                    v->write("fVerStreamOffset", &c->fVerStreamOffset);

                    v->write("nXYRecordSize", &c->nXYRecordSize);
                    v->write("fHorStreamScale", &c->fHorStreamScale);
                    v->write("fHorStreamOffset", &c->fHorStreamOffset);

                    v->write("bAutoSweep", &c->bAutoSweep);
                    v->write("nAutoSweepLimit", &c->nAutoSweepLimit);
                    v->write("nAutoSweepCounter", &c->nAutoSweepCounter);

                    v->write("enState", &c->enState);

                    v->write("nUpdate", &c->nUpdate);

                    v->begin_object("sStateStage", &c->sStateStage, sizeof(c->sStateStage));
                    {
                        v->write("nPV_pScpMode", &c->sStateStage.nPV_pScpMode);

                        v->write("nPV_pCoupling_x", &c->sStateStage.nPV_pCoupling_x);
                        v->write("nPV_pCoupling_y", &c->sStateStage.nPV_pCoupling_y);
                        v->write("nPV_pCoupling_ext", &c->sStateStage.nPV_pCoupling_ext);
                        v->write("nPV_pOvsMode", &c->sStateStage.nPV_pOvsMode);

                        v->write("nPV_pTrgInput", &c->sStateStage.nPV_pTrgInput);
                        v->write("fPV_pVerDiv", &c->sStateStage.fPV_pVerDiv);
                        v->write("fPV_pVerPos", &c->sStateStage.fPV_pVerPos);
                        v->write("fPV_pTrgLevel", &c->sStateStage.fPV_pTrgLevel);
                        v->write("fPV_pTrgHys", &c->sStateStage.fPV_pTrgHys);
                        v->write("nPV_pTrgMode", &c->sStateStage.nPV_pTrgMode);
                        v->write("fPV_pTrgHold", &c->sStateStage.fPV_pTrgHold);
                        v->write("nPV_pTrgType", &c->sStateStage.nPV_pTrgType);

                        v->write("fPV_pTimeDiv", &c->sStateStage.fPV_pTimeDiv);
                        v->write("fPV_pHorPos", &c->sStateStage.fPV_pHorPos);

                        v->write("nPV_pSweepType", &c->sStateStage.nPV_pSweepType);

                        v->write("fPV_pXYRecordTime", &c->sStateStage.fPV_pXYRecordTime);
                    }
                    v->end_object();

                    v->write("bUseGlobal", &c->bUseGlobal);
                    v->write("bFreeze", &c->bFreeze);

                    v->write("vIn_x", &c->vIn_x);
                    v->write("vIn_y", &c->vIn_y);
                    v->write("vIn_ext", &c->vIn_ext);

                    v->write("vOut_x", &c->vOut_x);
                    v->write("vOut_y", &c->vOut_y);

                    v->write("pIn_x", &c->pIn_x);
                    v->write("pIn_y", &c->pIn_y);
                    v->write("pIn_ext", &c->pIn_ext);

                    v->write("pOut_x", &c->pOut_x);
                    v->write("pOut_y", &c->pOut_y);

                    v->write("pOvsMode", &c->pOvsMode);
                    v->write("pScpMode", &c->pScpMode);
                    v->write("pCoupling_x", &c->pCoupling_x);
                    v->write("pCoupling_y", &c->pCoupling_y);
                    v->write("pCoupling_ext", &c->pCoupling_ext);

                    v->write("pSweepType", &c->pSweepType);
                    v->write("pTimeDiv", &c->pTimeDiv);
                    v->write("pHorDiv", &c->pHorDiv);
                    v->write("pHorPos", &c->pHorPos);

                    v->write("pVerDiv", &c->pVerDiv);
                    v->write("pVerPos", &c->pVerPos);

                    v->write("pTrgHys", &c->pTrgHys);
                    v->write("pTrgLev", &c->pTrgLev);
                    v->write("pTrgHold", &c->pTrgHold);
                    v->write("pTrgMode", &c->pTrgMode);
                    v->write("pTrgType", &c->pTrgType);
                    v->write("pTrgInput", &c->pTrgInput);
                    v->write("pTrgReset", &c->pTrgReset);

                    v->write("pGlobalSwitch", &c->pGlobalSwitch);
                    v->write("pFreezeSwitch", &c->pFreezeSwitch);
                    v->write("pSoloSwitch", &c->pSoloSwitch);
                    v->write("pMuteSwitch", &c->pMuteSwitch);

                    v->write("pStream", &c->pStream);
                }
                v->end_object();
            }
            v->end_array();

            v->write("pData", pData);

            v->write("pStrobeHistSize", pStrobeHistSize);
            v->write("pXYRecordTime", pXYRecordTime);
            v->write("pFreeze", pFreeze);

            v->write("pChannelSelector", pChannelSelector);

            v->write("pOvsMode", pOvsMode);
            v->write("pScpMode", pScpMode);
            v->write("pCoupling_x", pCoupling_x);
            v->write("pCoupling_y", pCoupling_y);
            v->write("pCoupling_ext", pCoupling_ext);

            v->write("pSweepType", pSweepType);
            v->write("pTimeDiv", pTimeDiv);
            v->write("pHorDiv", pHorDiv);
            v->write("pHorPos", pHorPos);

            v->write("pVerDiv", pVerDiv);
            v->write("pVerPos", pVerPos);

            v->write("pTrgHys", pTrgHys);
            v->write("pTrgLev", pTrgLev);
            v->write("pTrgHold", pTrgHold);
            v->write("pTrgMode", pTrgMode);
            v->write("pTrgType", pTrgType);
            v->write("pTrgInput", pTrgInput);
            v->write("pTrgReset", pTrgReset);

            v->write("pIDisplay", pIDisplay);
        }

        static const uint32_t ch_colors[] =
        {
            // x1
            0x0a9bff,
            // x2
            0xff0e11,
            0x0a9bff,
            // x4
            0xff0e11,
            0x12ff16,
            0xff6c11,
            0x0a9bff
        };

        bool oscilloscope::inline_display(plug::ICanvas *cv, size_t width, size_t height)
        {
            // Check proportions
            if (height > width)
                height  = width;

            // Init canvas
            if (!cv->init(width, height))
                return false;
            width   = cv->width();
            height  = cv->height();
            float cx    = width >> 1;
            float cy    = height >> 1;

            // Clear background
            cv->paint();

            // Draw axis
            cv->set_line_width(1.0);
            cv->set_color_rgb(CV_SILVER, 0.5f);
            cv->line(0, 0, width, height);
            cv->line(0, height, width, 0);

            cv->set_color_rgb(CV_WHITE, 0.5f);
            cv->line(cx, 0, cx, height);
            cv->line(0, cy, width, cy);

            // Check for solos:
            const uint32_t *cols =
                    (nChannels < 2) ? &ch_colors[0] :
                    (nChannels < 4) ? &ch_colors[1] :
                    &ch_colors[3];

            float halfv = 0.5f * width;
            float halfh = 0.5f * height;

            // Estimate the display length
            size_t di_length = 1;
            for (size_t ch = 0; ch < nChannels; ++ch)
                di_length = lsp_max(di_length, vChannels[ch].nIDisplay);

            // Allocate buffer: t, f(t)
            pIDisplay = core::IDBuffer::reuse(pIDisplay, 2, di_length);
            core::IDBuffer *b = pIDisplay;
            if (b == NULL)
                return false;

            bool aa = cv->set_anti_aliasing(true);

            for (size_t ch = 0; ch < nChannels; ++ch)
            {
                channel_t *c = &vChannels[ch];
                if (!c->bVisible)
                    continue;

                size_t dlen = lsp_min(c->nIDisplay, di_length);
                for (size_t i=0; i<dlen; ++i)
                {
                    b->v[0][i] = halfv * (c->vIDisplay_x[i] + 1.0f);
                    b->v[1][i] = halfh * (-c->vIDisplay_y[i] + 1.0f);
                }

                // Set color and draw
                cv->set_color_rgb(cols[ch]);
                cv->set_line_width(2);
                cv->draw_lines(b->v[0], b->v[1], dlen);
            }

            cv->set_anti_aliasing(aa);

            return true;
        }
    } /* namespace plugins */
} /* namespace lsp */


