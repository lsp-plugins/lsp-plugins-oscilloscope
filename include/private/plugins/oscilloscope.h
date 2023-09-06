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

#ifndef PRIVATE_PLUGINS_OSCILLOSCOPE_H_
#define PRIVATE_PLUGINS_OSCILLOSCOPE_H_

#include <lsp-plug.in/plug-fw/plug.h>
#include <lsp-plug.in/plug-fw/core/IDBuffer.h>
#include <lsp-plug.in/dsp-units/filters/FilterBank.h>
#include <lsp-plug.in/dsp-units/util/Delay.h>
#include <lsp-plug.in/dsp-units/util/Oversampler.h>
#include <lsp-plug.in/dsp-units/util/Oscillator.h>
#include <lsp-plug.in/dsp-units/util/Trigger.h>

#include <private/meta/oscilloscope.h>

namespace lsp
{
    namespace plugins
    {
        /**
         * Oscilloscope Plugin Series
         */
        class oscilloscope: public plug::Module
        {
            protected:

                enum ch_update_t
                {
                    UPD_SCPMODE             = 1 << 0,

                    UPD_ACBLOCK_X           = 1 << 1,
                    UPD_ACBLOCK_Y           = 1 << 2,
                    UPD_ACBLOCK_EXT         = 1 << 3,

                    UPD_OVERSAMPLER_X       = 1 << 4,
                    UPD_OVERSAMPLER_Y       = 1 << 5,
                    UPD_OVERSAMPLER_EXT     = 1 << 6,

                    UPD_XY_RECORD_TIME      = 1 << 7,

                    UPD_HOR_SCALES          = 1 << 8,

                    UPD_PRETRG_DELAY        = 1 << 9,

                    UPD_SWEEP_GENERATOR     = 1 << 10,

                    UPD_VER_SCALES          = 1 << 11,

                    UPD_TRIGGER_INPUT       = 1 << 12,
                    UPD_TRIGGER_HOLD        = 1 << 13,
                    UPD_TRIGGER             = 1 << 14,
                    UPD_TRGGER_RESET        = 1 << 15
                };

                enum ch_mode_t
                {
                    CH_MODE_XY,
                    CH_MODE_TRIGGERED,
                    CH_MODE_GONIOMETER,

                    CH_MODE_DFL = CH_MODE_TRIGGERED
                };

                enum ch_sweep_type_t
                {
                    CH_SWEEP_TYPE_SAWTOOTH,
                    CH_SWEEP_TYPE_TRIANGULAR,
                    CH_SWEEP_TYPE_SINE,

                    CH_SWEEP_TYPE_DFL = CH_SWEEP_TYPE_SAWTOOTH
                };

                enum ch_trg_input_t
                {
                    CH_TRG_INPUT_Y,
                    CH_TRG_INPUT_EXT,

                    CH_TRG_INPUT_DFL = CH_TRG_INPUT_Y
                };

                enum ch_coupling_t
                {
                    CH_COUPLING_AC,
                    CH_COUPLING_DC,

                    CH_COUPLING_DFL = CH_COUPLING_DC
                };

                enum ch_state_t
                {
                    CH_STATE_LISTENING,
                    CH_STATE_SWEEPING
                };

                typedef struct dc_block_t
                {
                    float   fAlpha;
                    float   fGain;
                } dc_block_t;

                typedef struct ch_state_stage_t
                {
                    size_t  nPV_pScpMode;

                    size_t  nPV_pCoupling_x;
                    size_t  nPV_pCoupling_y;
                    size_t  nPV_pCoupling_ext;
                    size_t  nPV_pOvsMode;

                    size_t  nPV_pTrgInput;
                    float   fPV_pVerDiv;
                    float   fPV_pVerPos;
                    float   fPV_pTrgLevel;
                    float   fPV_pTrgHys;
                    size_t  nPV_pTrgMode;
                    float   fPV_pTrgHold;
                    size_t  nPV_pTrgType;

                    float   fPV_pTimeDiv;
                    float   fPV_pHorDiv;
                    float   fPV_pHorPos;

                    size_t  nPV_pSweepType;

                    float   fPV_pXYRecordTime;
                } ch_state_stage_t;

                typedef struct channel_t
                {
                    ch_mode_t               enMode;
                    ch_sweep_type_t         enSweepType;
                    ch_trg_input_t          enTrgInput;
                    ch_coupling_t           enCoupling_x;
                    ch_coupling_t           enCoupling_y;
                    ch_coupling_t           enCoupling_ext;

                    dspu::FilterBank        sDCBlockBank_x;
                    dspu::FilterBank        sDCBlockBank_y;
                    dspu::FilterBank        sDCBlockBank_ext;

                    dspu::over_mode_t       enOverMode;
                    size_t                  nOversampling;
                    size_t                  nOverSampleRate;

                    dspu::Oversampler       sOversampler_x;
                    dspu::Oversampler       sOversampler_y;
                    dspu::Oversampler       sOversampler_ext;

                    dspu::Delay             sPreTrgDelay;

                    dspu::Trigger           sTrigger;

                    dspu::Oscillator        sSweepGenerator;

                    float                  *vTemp;
                    float                  *vData_x;
                    float                  *vData_y;
                    float                  *vData_ext;
                    float                  *vData_y_delay;
                    float                  *vDisplay_x;
                    float                  *vDisplay_y;
                    float                  *vDisplay_s; // Strobe

                    float                  *vIDisplay_x;
                    float                  *vIDisplay_y;
                    size_t                  nIDisplay;

                    size_t                  nDataHead;
                    size_t                  nDisplayHead;
                    size_t                  nSamplesCounter;
                    bool                    bClearStream;

                    size_t                  nPreTrigger;
                    size_t                  nSweepSize;

                    float                   fVerStreamScale;
                    float                   fVerStreamOffset;

                    size_t                  nXYRecordSize;
                    float                   fHorStreamScale;
                    float                   fHorStreamOffset;

                    bool                    bAutoSweep;
                    size_t                  nAutoSweepLimit;
                    size_t                  nAutoSweepCounter;

                    ch_state_t              enState;

                    size_t                  nUpdate;
                    ch_state_stage_t        sStateStage;
                    bool                    bUseGlobal;
                    bool                    bFreeze;
                    bool                    bVisible;

                    float                  *vIn_x;
                    float                  *vIn_y;
                    float                  *vIn_ext;

                    float                  *vOut_x;
                    float                  *vOut_y;

                    plug::IPort            *pIn_x;
                    plug::IPort            *pIn_y;
                    plug::IPort            *pIn_ext;

                    plug::IPort            *pOut_x;
                    plug::IPort            *pOut_y;

                    plug::IPort            *pOvsMode;
                    plug::IPort            *pScpMode;
                    plug::IPort            *pCoupling_x;
                    plug::IPort            *pCoupling_y;
                    plug::IPort            *pCoupling_ext;

                    plug::IPort            *pSweepType;
                    plug::IPort            *pTimeDiv;
                    plug::IPort            *pHorDiv;
                    plug::IPort            *pHorPos;

                    plug::IPort            *pVerDiv;
                    plug::IPort            *pVerPos;

                    plug::IPort            *pTrgHys;
                    plug::IPort            *pTrgLev;
                    plug::IPort            *pTrgHold;
                    plug::IPort            *pTrgMode;
                    plug::IPort            *pTrgType;
                    plug::IPort            *pTrgInput;
                    plug::IPort            *pTrgReset;

                    plug::IPort            *pGlobalSwitch;
                    plug::IPort            *pFreezeSwitch;
                    plug::IPort            *pSoloSwitch;
                    plug::IPort            *pMuteSwitch;

                    plug::IPort            *pStream;
                } channel_t;

            protected:
                dc_block_t          sDCBlockParams;
                size_t              nChannels;
                channel_t          *vChannels;
                uint8_t            *pData;

                // Common Controls
                plug::IPort        *pStrobeHistSize;
                plug::IPort        *pXYRecordTime;
                plug::IPort        *pFreeze;

                // Channel Selector
                plug::IPort        *pChannelSelector;

                // Global ports:
                plug::IPort        *pOvsMode;
                plug::IPort        *pScpMode;
                plug::IPort        *pCoupling_x;
                plug::IPort        *pCoupling_y;
                plug::IPort        *pCoupling_ext;

                plug::IPort        *pSweepType;
                plug::IPort        *pTimeDiv;
                plug::IPort        *pHorDiv;
                plug::IPort        *pHorPos;

                plug::IPort        *pVerDiv;
                plug::IPort        *pVerPos;

                plug::IPort        *pTrgHys;
                plug::IPort        *pTrgLev;
                plug::IPort        *pTrgHold;
                plug::IPort        *pTrgMode;
                plug::IPort        *pTrgType;
                plug::IPort        *pTrgInput;
                plug::IPort        *pTrgReset;

                core::IDBuffer     *pIDisplay;      // Inline display buffer

            protected:
                static dspu::over_mode_t   get_oversampler_mode(size_t portValue);
                static ch_mode_t           get_scope_mode(size_t portValue);
                static ch_sweep_type_t     get_sweep_type(size_t portValue);
                static ch_trg_input_t      get_trigger_input(size_t portValue);
                static ch_coupling_t       get_coupling_type(size_t portValue);
                static dspu::trg_mode_t    get_trigger_mode(size_t portValue);
                static dspu::trg_type_t    get_trigger_type(size_t portValue);

            protected:
                void                update_dc_block_filter(dspu::FilterBank &rFilterBank);
                void                reconfigure_dc_block_filters();
                void                do_sweep_step(channel_t *c, float strobe_value);
                float              *select_trigger_input(float *extPtr, float* yPtr, ch_trg_input_t input);
                inline void         set_oversampler(dspu::Oversampler &over, dspu::over_mode_t mode);
                inline void         set_sweep_generator(channel_t *c);
                inline void         configure_oversamplers(channel_t *c, dspu::over_mode_t mode);
                void                init_state_stage(channel_t *c);
                void                commit_staged_state_change(channel_t *c);
                bool                graph_stream(channel_t *c);
                void                do_destroy();

            public:
                explicit oscilloscope(const meta::plugin_t *metadata, size_t channels);
                virtual ~oscilloscope() override;

                virtual void        init(plug::IWrapper *wrapper, plug::IPort **ports) override;
                virtual void        destroy() override;

            public:
                virtual void        update_settings() override;
                virtual void        update_sample_rate(long sr) override;

                virtual void        process(size_t samples) override;

                virtual void        dump(dspu::IStateDumper *v) const override;

                virtual bool        inline_display(plug::ICanvas *cv, size_t width, size_t height) override;
        };
    } /* namespace plugins */
} /* namespace lsp */

#endif /* PRIVATE_PLUGINS_OSCILLOSCOPE_H_ */
