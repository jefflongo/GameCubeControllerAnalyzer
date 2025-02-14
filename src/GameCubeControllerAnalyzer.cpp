#include "GameCubeControllerAnalyzer.h"

#include "GameCubeControllerAnalyzerSettings.h"

#include <AnalyzerChannelData.h>

GameCubeControllerAnalyzer::GameCubeControllerAnalyzer()
    : Analyzer2(), mSettings( new GameCubeControllerAnalyzerSettings() ), mSimulationInitilized( false )
{
    SetAnalyzerSettings( mSettings.get() );
    UseFrameV2();
}

GameCubeControllerAnalyzer::~GameCubeControllerAnalyzer()
{
    KillThread();
}

void GameCubeControllerAnalyzer::SetupResults()
{
    mResults.reset( new GameCubeControllerAnalyzerResults( this, mSettings.get() ) );
    SetAnalyzerResults( mResults.get() );
    mResults->AddChannelBubblesWillAppearOn( mSettings->mInputChannel );
}

void GameCubeControllerAnalyzer::WorkerThread()
{
    mSampleRateHz = GetSampleRate();

    mGamecube = GetAnalyzerChannelData( mSettings->mInputChannel );

    AdvanceToEndOfPacket();

    while( true )
    {
        DecodeFrames();
        ReportProgress( mGamecube->GetSampleNumber() );
        CheckIfThreadShouldExit();
    }
}

bool GameCubeControllerAnalyzer::NeedsRerun()
{
    return false;
}

U32 GameCubeControllerAnalyzer::GenerateSimulationData( U64 minimum_sample_index, U32 device_sample_rate,
                                                        SimulationChannelDescriptor** simulation_channels )
{
    if( mSimulationInitilized == false )
    {
        mSimulationDataGenerator.Initialize( GetSimulationSampleRate(), mSettings.get() );
        mSimulationInitilized = true;
    }

    return mSimulationDataGenerator.GenerateSimulationData( minimum_sample_index, device_sample_rate, simulation_channels );
}

U32 GameCubeControllerAnalyzer::GetMinimumSampleRateHz()
{
    return 2000000;
}

const char* GameCubeControllerAnalyzer::GetAnalyzerName() const
{
    return "GameCube";
}

const char* GetAnalyzerName()
{
    return "GameCube";
}

Analyzer* CreateAnalyzer()
{
    return new GameCubeControllerAnalyzer();
}

void DestroyAnalyzer( Analyzer* analyzer )
{
    delete analyzer;
}

U64 GameCubeControllerAnalyzer::GetPulseWidthNs( U64 start_edge, U64 end_edge )
{
    return static_cast<U64>( ( end_edge - start_edge ) * 1000000000 / mSampleRateHz );
}

// advances to the rising edge at the end of a packet
void GameCubeControllerAnalyzer::AdvanceToEndOfPacket()
{
    if( mGamecube->GetBitState() == BIT_LOW )
    {
        mGamecube->AdvanceToNextEdge();
    }

    // if a complete packet was received successfully, we're already at the end of the packet
    if( mDecodedReception )
    {
        mDecodedReception = false;
        return;
    }

    // otherwise, something was corrupted. synchronize to at least 100us of inactivity.
    // this way, we can be sure we're at the beginning of a transmission and not in between
    // a transmission and reception
    while( GetPulseWidthNs( mGamecube->GetSampleNumber(), mGamecube->GetSampleOfNextEdge() ) < 100000 )
    {
        mGamecube->AdvanceToNextEdge();
        mGamecube->AdvanceToNextEdge();
    }
}

// advances to the falling edge of the next bit in a packet
bool GameCubeControllerAnalyzer::AdvanceToNextBitInPacket()
{
    // if the transmission from the host completed, the controller has ~100us to respond
    // in this condition, provide the extra leniency
    int duration = mDecodedTransmission ? 100000 : 5000;
    mDecodedTransmission = false;

    if( GetPulseWidthNs( mGamecube->GetSampleNumber(), mGamecube->GetSampleOfNextEdge() ) < duration )
    {
        mGamecube->AdvanceToNextEdge();
        return true;
    }

    return false;
}

void GameCubeControllerAnalyzer::DecodeFrames()
{
    // traverse to the first falling edge
    mGamecube->AdvanceToNextEdge();
    U64 start_sample = mGamecube->GetSampleNumber();

    U8 cmd, data;

    // try to decode the command
    if( !DecodeByte( cmd ) )
    {
        AdvanceToEndOfPacket();
        return;
    }

    bool ok = true;
    FrameV2 frame_v2;
    // TODO: delete when FrameV2 supports bubble generation
    Frame frame;
    frame.mStartingSampleInclusive = start_sample;
    frame.mType = cmd;

    // TODO: support more commands, there is a list here: https://n64brew.dev/wiki/Joybus_Protocol
    switch( cmd )
    {
    case JoyBusCommand::CMD_ID:
    {
        // command stop bit
        if( !( AdvanceToNextBitInPacket() && DecodeStopBit() ) )
        {
            AdvanceToEndOfPacket();
            return;
        }
        mDecodedTransmission = ok;

        // response
        uint8_t device[ 2 ];
        if( ok )
            ok = AdvanceToNextBitInPacket() && DecodeByte( data );
        if( ok )
        {
            device[ 1 ] = data;
        }
        if( ok )
            ok = AdvanceToNextBitInPacket() && DecodeByte( data );
        if( ok )
        {
            device[ 0 ] = data;
            frame_v2.AddByteArray( "Device", device, sizeof( device ) );
        }
        if( ok )
            ok = AdvanceToNextBitInPacket() && DecodeByte( data );
        if( ok )
        {
            frame_v2.AddByte( "Status", data );
        }
        if( ok )
            ok = AdvanceToNextBitInPacket() && DecodeStopBit();
        mDecodedReception = ok;
        AdvanceToEndOfPacket();

        U64 end_sample = mGamecube->GetSampleNumber();
        frame.mEndingSampleInclusive = end_sample;
        mResults->AddFrame( frame );
        mResults->AddFrameV2( frame_v2, "id", start_sample, end_sample );
        mResults->CommitResults();
    }
    break;

    case JoyBusCommand::CMD_STATUS:
    {
        // command arg1
        if( !( AdvanceToNextBitInPacket() && DecodeByte( data ) ) )
        {
            AdvanceToEndOfPacket();
            return;
        }
        frame_v2.AddByte( "Poll Mode", data );
        U8 poll_mode = data;

        // command arg2
        if( !( AdvanceToNextBitInPacket() && DecodeByte( data ) ) )
        {
            AdvanceToEndOfPacket();
            return;
        }
        frame_v2.AddByte( "Motor Mode", data );

        // command stop bit
        if( !( AdvanceToNextBitInPacket() && DecodeStopBit() ) )
        {
            AdvanceToEndOfPacket();
            return;
        }
        mDecodedTransmission = ok;

        // response
        uint8_t buttons[ 2 ];
        if( ok )
            ok = AdvanceToNextBitInPacket() && DecodeByte( data );
        if( ok )
        {
            buttons[ 1 ] = data;
        }
        if( ok )
            ok = AdvanceToNextBitInPacket() && DecodeByte( data );
        if( ok )
        {
            buttons[ 0 ] = data;
            frame_v2.AddByteArray( "Buttons", buttons, sizeof( buttons ) );
        }
        if( ok )
            ok = AdvanceToNextBitInPacket() && DecodeByte( data );
        if( ok )
        {
            frame_v2.AddByte( "Joystick X", data );
        }
        if( ok )
            ok = AdvanceToNextBitInPacket() && DecodeByte( data );
        if( ok )
        {
            frame_v2.AddByte( "Joystick Y", data );
        }
        if( ok )
            ok = AdvanceToNextBitInPacket() && DecodeByte( data );
        if( ok )
        {
            if( poll_mode == 1 || poll_mode == 2 )
            {
                frame_v2.AddByte( "C-Stick X", data & 0xF0 );
                frame_v2.AddByte( "C-Stick Y", data & 0x0F );
            }
            else
            {
                frame_v2.AddByte( "C-Stick X", data );
            }
        }
        if( ok )
            ok = AdvanceToNextBitInPacket() && DecodeByte( data );
        if( ok )
        {
            if( poll_mode == 1 )
            {
                frame_v2.AddByte( "L Analog", data );
            }
            else if( poll_mode == 2 )
            {
                frame_v2.AddByte( "L Analog", data & 0xF0 );
                frame_v2.AddByte( "R Analog", data & 0x0F );
            }
            else
            {
                frame_v2.AddByte( "C-Stick Y", data );
            }
        }
        if( ok )
            ok = AdvanceToNextBitInPacket() && DecodeByte( data );
        if( ok )
        {
            if( poll_mode == 0 )
            {
                frame_v2.AddByte( "L Analog", data & 0xF0 );
                frame_v2.AddByte( "R Analog", data & 0x0F );
            }
            else if( poll_mode == 1 )
            {
                frame_v2.AddByte( "R Analog", data );
            }
            else if( poll_mode == 2 || poll_mode == 4 )
            {
                frame_v2.AddByte( "A Analog", data );
            }
            else
            {
                frame_v2.AddByte( "L Analog", data );
            }
        }
        if( ok )
            ok = AdvanceToNextBitInPacket() && DecodeByte( data );
        if( ok )
        {
            if( poll_mode == 0 || poll_mode == 1 )
            {
                frame_v2.AddByte( "A Analog", data & 0xF0 );
                frame_v2.AddByte( "B Analog", data & 0x0F );
            }
            else if( poll_mode == 2 || poll_mode == 4 )
            {
                frame_v2.AddByte( "B Analog", data );
            }
            else
            {
                frame_v2.AddByte( "R Analog", data );
            }
        }

        if( ok )
            ok = AdvanceToNextBitInPacket() && DecodeStopBit();
        mDecodedReception = ok;
        AdvanceToEndOfPacket();

        U64 end_sample = mGamecube->GetSampleNumber();
        frame.mEndingSampleInclusive = end_sample;
        mResults->AddFrame( frame );
        mResults->AddFrameV2( frame_v2, "status", start_sample, end_sample );
        mResults->CommitResults();
    }
    break;

    case JoyBusCommand::CMD_ORIGIN:
    {
        // command stop bit
        if( !( AdvanceToNextBitInPacket() && DecodeStopBit() ) )
        {
            AdvanceToEndOfPacket();
            return;
        }
        mDecodedTransmission = ok;

        // response
        uint8_t buttons[ 2 ];
        if( ok )
            ok = AdvanceToNextBitInPacket() && DecodeByte( data );
        if( ok )
        {
            buttons[ 1 ] = data;
        }
        if( ok )
            ok = AdvanceToNextBitInPacket() && DecodeByte( data );
        if( ok )
        {
            buttons[ 0 ] = data;
            frame_v2.AddByteArray( "Buttons", buttons, sizeof( buttons ) );
        }
        if( ok )
            ok = AdvanceToNextBitInPacket() && DecodeByte( data );
        if( ok )
        {
            frame_v2.AddByte( "Joystick X", data );
        }
        if( ok )
            ok = AdvanceToNextBitInPacket() && DecodeByte( data );
        if( ok )
        {
            frame_v2.AddByte( "Joystick Y", data );
        }
        if( ok )
            ok = AdvanceToNextBitInPacket() && DecodeByte( data );
        if( ok )
        {
            frame_v2.AddByte( "C-Stick X", data );
        }
        if( ok )
            ok = AdvanceToNextBitInPacket() && DecodeByte( data );
        if( ok )
        {
            frame_v2.AddByte( "C-Stick Y", data );
        }
        if( ok )
            ok = AdvanceToNextBitInPacket() && DecodeByte( data );
        if( ok )
        {
            frame_v2.AddByte( "L Analog", data );
        }
        if( ok )
            ok = AdvanceToNextBitInPacket() && DecodeByte( data );
        if( ok )
        {
            frame_v2.AddByte( "R Analog", data );
        }
        if( ok )
            ok = AdvanceToNextBitInPacket() && DecodeByte( data );
        if( ok )
        {
            frame_v2.AddByte( "A Analog", data );
        }
        if( ok )
            ok = AdvanceToNextBitInPacket() && DecodeByte( data );
        if( ok )
        {
            frame_v2.AddByte( "B Analog", data );
        }

        if( ok )
            ok = AdvanceToNextBitInPacket() && DecodeStopBit();
        mDecodedReception = ok;
        AdvanceToEndOfPacket();

        U64 end_sample = mGamecube->GetSampleNumber();
        frame.mEndingSampleInclusive = end_sample;
        mResults->AddFrame( frame );
        mResults->AddFrameV2( frame_v2, "origin", start_sample, end_sample );
        mResults->CommitResults();
    }
    break;


    case JoyBusCommand::CMD_RECALIBRATE:
    {
        // command arg1
        if( !( AdvanceToNextBitInPacket() && DecodeByte( data ) ) )
        {
            AdvanceToEndOfPacket();
            return;
        }
        frame_v2.AddByte( "Poll Mode", data );

        // command arg2
        if( !( AdvanceToNextBitInPacket() && DecodeByte( data ) ) )
        {
            AdvanceToEndOfPacket();
            return;
        }
        frame_v2.AddByte( "Motor Mode", data );

        // command stop bit
        if( !( AdvanceToNextBitInPacket() && DecodeStopBit() ) )
        {
            AdvanceToEndOfPacket();
            return;
        }
        mDecodedTransmission = ok;

        // response
        uint8_t buttons[ 2 ];
        if( ok )
            ok = AdvanceToNextBitInPacket() && DecodeByte( data );
        if( ok )
        {
            buttons[ 1 ] = data;
        }
        if( ok )
            ok = AdvanceToNextBitInPacket() && DecodeByte( data );
        if( ok )
        {
            buttons[ 0 ] = data;
            frame_v2.AddByteArray( "Buttons", buttons, sizeof( buttons ) );
        }
        if( ok )
            ok = AdvanceToNextBitInPacket() && DecodeByte( data );
        if( ok )
        {
            frame_v2.AddByte( "Joystick X", data );
        }
        if( ok )
            ok = AdvanceToNextBitInPacket() && DecodeByte( data );
        if( ok )
        {
            frame_v2.AddByte( "Joystick Y", data );
        }
        if( ok )
            ok = AdvanceToNextBitInPacket() && DecodeByte( data );
        if( ok )
        {
            frame_v2.AddByte( "C-Stick X", data );
        }
        if( ok )
            ok = AdvanceToNextBitInPacket() && DecodeByte( data );
        if( ok )
        {
            frame_v2.AddByte( "C-Stick Y", data );
        }
        if( ok )
            ok = AdvanceToNextBitInPacket() && DecodeByte( data );
        if( ok )
        {
            frame_v2.AddByte( "L Analog", data );
        }
        if( ok )
            ok = AdvanceToNextBitInPacket() && DecodeByte( data );
        if( ok )
        {
            frame_v2.AddByte( "R Analog", data );
        }
        if( ok )
            ok = AdvanceToNextBitInPacket() && DecodeByte( data );
        if( ok )
        {
            frame_v2.AddByte( "A Analog", data );
        }
        if( ok )
            ok = AdvanceToNextBitInPacket() && DecodeByte( data );
        if( ok )
        {
            frame_v2.AddByte( "B Analog", data );
        }

        if( ok )
            ok = AdvanceToNextBitInPacket() && DecodeStopBit();
        mDecodedReception = ok;
        AdvanceToEndOfPacket();

        U64 end_sample = mGamecube->GetSampleNumber();
        frame.mEndingSampleInclusive = end_sample;
        mResults->AddFrame( frame );
        mResults->AddFrameV2( frame_v2, "recalibrate", start_sample, end_sample );
        mResults->CommitResults();
    }
    break;

    case JoyBusCommand::CMD_STATUS_LONG:
    {
        // command arg1
        if( !( AdvanceToNextBitInPacket() && DecodeByte( data ) ) )
        {
            AdvanceToEndOfPacket();
            return;
        }
        frame_v2.AddByte( "Poll Mode", data );

        // command arg2
        if( !( AdvanceToNextBitInPacket() && DecodeByte( data ) ) )
        {
            AdvanceToEndOfPacket();
            return;
        }
        frame_v2.AddByte( "Motor Mode", data );

        // command stop bit
        if( !( AdvanceToNextBitInPacket() && DecodeStopBit() ) )
        {
            AdvanceToEndOfPacket();
            return;
        }
        mDecodedTransmission = ok;

        // response
        uint8_t buttons[ 2 ];
        if( ok )
            ok = AdvanceToNextBitInPacket() && DecodeByte( data );
        if( ok )
        {
            buttons[ 1 ] = data;
        }
        if( ok )
            ok = AdvanceToNextBitInPacket() && DecodeByte( data );
        if( ok )
        {
            buttons[ 0 ] = data;
            frame_v2.AddByteArray( "Buttons", buttons, sizeof( buttons ) );
        }
        if( ok )
            ok = AdvanceToNextBitInPacket() && DecodeByte( data );
        if( ok )
        {
            frame_v2.AddByte( "Joystick X", data );
        }
        if( ok )
            ok = AdvanceToNextBitInPacket() && DecodeByte( data );
        if( ok )
        {
            frame_v2.AddByte( "Joystick Y", data );
        }
        if( ok )
            ok = AdvanceToNextBitInPacket() && DecodeByte( data );
        if( ok )
        {
            frame_v2.AddByte( "C-Stick X", data );
        }
        if( ok )
            ok = AdvanceToNextBitInPacket() && DecodeByte( data );
        if( ok )
        {
            frame_v2.AddByte( "C-Stick Y", data );
        }
        if( ok )
            ok = AdvanceToNextBitInPacket() && DecodeByte( data );
        if( ok )
        {
            frame_v2.AddByte( "L Analog", data );
        }
        if( ok )
            ok = AdvanceToNextBitInPacket() && DecodeByte( data );
        if( ok )
        {
            frame_v2.AddByte( "R Anlog", data );
        }
        if( ok )
            ok = AdvanceToNextBitInPacket() && DecodeByte( data );
        if( ok )
        {
            frame_v2.AddByte( "A Analog", data );
        }
        if( ok )
            ok = AdvanceToNextBitInPacket() && DecodeByte( data );
        if( ok )
        {
            frame_v2.AddByte( "B Analog", data );
        }

        if( ok )
            ok = AdvanceToNextBitInPacket() && DecodeStopBit();
        mDecodedReception = ok;
        AdvanceToEndOfPacket();

        U64 end_sample = mGamecube->GetSampleNumber();
        frame.mEndingSampleInclusive = end_sample;
        mResults->AddFrame( frame );
        mResults->AddFrameV2( frame_v2, "status (long)", start_sample, end_sample );
        mResults->CommitResults();
    }
    break;

    default:
        AdvanceToEndOfPacket();
        break;
    }
}

// attempts to decode a byte. the current sample should be a falling edge and this
// function will return on a rising edge
bool GameCubeControllerAnalyzer::DecodeByte( U8& byte )
{
    byte = 0;
    for( U8 i = 0; i < 8; i++ )
    {
        bool bit;
        if( !DecodeDataBit( bit ) )
        {
            return false;
        }

        byte |= bit << ( 7 - i );

        if( i < 7 )
        {
            // advance to the next falling edge iff
            // - there are more bits to process in the current byte
            // - the last bit was successful
            mGamecube->AdvanceToNextEdge();
        }
    }

    return true;
}

// attempts to decode a single bit. on entry, the current sample should be a falling edge and this
// function will return on a rising edge
bool GameCubeControllerAnalyzer::DecodeDataBit( bool& bit )
{
    U64 starting_sample, ending_sample, rising_edge_sample, falling_edge_sample;

    // determine whether the bit is a 1 or 0 based on the duration of the low time
    starting_sample = falling_edge_sample = mGamecube->GetSampleNumber();
    mGamecube->AdvanceToNextEdge();
    rising_edge_sample = mGamecube->GetSampleNumber();

    U64 low_time = GetPulseWidthNs( falling_edge_sample, rising_edge_sample );

    if( low_time >= 5000 )
    {
        return false;
    }
    else
    {
        bit = low_time < 2000;

        // make sure the high time is reasonable. peek at the next falling edge, but don't
        // actually advance to it yet, in case something is wrong.
        ending_sample = falling_edge_sample = mGamecube->GetSampleOfNextEdge();
        U64 high_time = GetPulseWidthNs( rising_edge_sample, falling_edge_sample );

        if( high_time >= 5000 )
        {
            return false;
        }

        // add an indicator showing the bit value
        U64 middle_sample = ( starting_sample + ending_sample ) / 2;
        mResults->AddMarker( middle_sample, AnalyzerResults::Dot, mSettings->mInputChannel );
    }

    return true;
}

// attempt to detect a stop bit, which is a single "1" bit where the high time doesn't matter.
// on entry, the current sample should be a falling edge and this function will return on a rising
// edge
bool GameCubeControllerAnalyzer::DecodeStopBit()
{
    U64 falling_edge_sample = mGamecube->GetSampleNumber();
    mGamecube->AdvanceToNextEdge();
    U64 rising_edge_sample = mGamecube->GetSampleNumber();

    U64 low_time = GetPulseWidthNs( falling_edge_sample, rising_edge_sample );

    // after observing an OEM controller, the low-time of a stop bit tended to be more than an
    // average "1" but less than a "0". therefore, we add a bit of leniency.
    return low_time < 2500;
}
