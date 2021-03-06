#include "ofxAudioUnit.h"
#include "ofxAudioUnitUtils.h"
#include "TPCircularBuffer.h"

AudioComponentDescription inputDesc = {
	kAudioUnitType_Output,
#if TARGET_OS_IPHONE
	kAudioUnitSubType_RemoteIO,
#else
	kAudioUnitSubType_HALOutput,
#endif
	kAudioUnitManufacturer_Apple
};

// Handles rendering audio from input unit to internal buffer
OSStatus RenderCallback(void *inRefCon,
						AudioUnitRenderActionFlags *ioActionFlags,
						const AudioTimeStamp *inTimeStamp,
						UInt32 inBusNumber,
						UInt32 inNumberFrames,
						AudioBufferList *ioData);

// Handles pulling audio from internal buffer to a downstream audio unit
OSStatus PullCallback(void *inRefCon,
					  AudioUnitRenderActionFlags *ioActionFlags,
					  const AudioTimeStamp *inTimeStamp,
					  UInt32 inBusNumber,
					  UInt32 inNumberFrames,
					  AudioBufferList *ioData);

typedef ofPtr<AudioBufferList> AudioBufferListRef;

struct InputContext
{
	vector<TPCircularBuffer> circularBuffers;
	AudioUnitRef inputUnit;
	AudioBufferListRef bufferList;
};

struct ofxAudioUnitInput::InputImpl
{
	InputContext ctx;
};

#pragma mark - ofxAudioUnitInput

// ----------------------------------------------------------
ofxAudioUnitInput::ofxAudioUnitInput(unsigned int samplesToBuffer)
: _isReady(false)
, _impl(new InputImpl)
// ----------------------------------------------------------
{
	_desc = inputDesc;
	initUnit();
	
	AudioStreamBasicDescription ASBD = {0};
	UInt32 ASBD_size = sizeof(ASBD);
	OFXAU_PRINT(AudioUnitGetProperty(*_unit,
									 kAudioUnitProperty_StreamFormat,
									 kAudioUnitScope_Output,
									 1,
									 &ASBD,
									 &ASBD_size),
				"getting input ASBD");
	
	_impl->ctx.inputUnit  = _unit;
	_impl->ctx.bufferList = AudioBufferListRef(AudioBufferListAlloc(ASBD.mChannelsPerFrame, 1024), AudioBufferListRelease);
	_impl->ctx.circularBuffers.resize(ASBD.mChannelsPerFrame);
	
	for(int i = 0; i < ASBD.mChannelsPerFrame; i++) {
		TPCircularBufferInit(&_impl->ctx.circularBuffers[i], samplesToBuffer * sizeof(AudioUnitSampleType));
	}
}

// ----------------------------------------------------------
ofxAudioUnitInput::~ofxAudioUnitInput()
// ----------------------------------------------------------
{
	cout << "STOP!" << endl;
	stop();
	
	for(int i = 0; i < _impl->ctx.circularBuffers.size(); i++) {
		TPCircularBufferCleanup(&_impl->ctx.circularBuffers[i]);
	}
}

#pragma mark - Connections

// ----------------------------------------------------------
ofxAudioUnit& ofxAudioUnitInput::connectTo(ofxAudioUnit &otherUnit, int destinationBus, int sourceBus)
// ----------------------------------------------------------
{
	AudioStreamBasicDescription ASBD;
	UInt32 ASBDSize = sizeof(ASBD);
	
	OFXAU_PRINT(AudioUnitGetProperty(otherUnit,
									 kAudioUnitProperty_StreamFormat,
									 kAudioUnitScope_Input,
									 destinationBus,
									 &ASBD,
									 &ASBDSize),
				"getting hardware input destination's format");
	
	OFXAU_PRINT(AudioUnitSetProperty(*_unit,
									 kAudioUnitProperty_StreamFormat,
									 kAudioUnitScope_Output,
									 1,
									 &ASBD,
									 sizeof(ASBD)),
				"setting hardware input's output format");
	
	AURenderCallbackStruct callback = {PullCallback, &_impl->ctx};
	otherUnit.setRenderCallback(callback, destinationBus);
	return otherUnit;
}

#pragma mark - Start / Stop

// ----------------------------------------------------------
bool ofxAudioUnitInput::start(int deviceID)
// ----------------------------------------------------------
{
    cout<<"ofxAudioUnitInput::start() "<<deviceID<< endl;
    
	if(!_isReady) _isReady = configureInputDevice(deviceID);
	if(!_isReady) return false;
	
	OFXAU_RET_BOOL(AudioOutputUnitStart(*_unit), "starting hardware input unit");
}

// ----------------------------------------------------------
bool ofxAudioUnitInput::stop()
// ----------------------------------------------------------
{
	OFXAU_RET_BOOL(AudioOutputUnitStop(*_unit), "stopping hardware input unit");
}

#pragma mark - Configuration

// ----------------------------------------------------------
bool ofxAudioUnitInput::configureInputDevice(int deviceID)
// ----------------------------------------------------------
{
	UInt32 on  = 1;
	OFXAU_RET_FALSE(AudioUnitSetProperty(*_unit,
										 kAudioOutputUnitProperty_EnableIO,
										 kAudioUnitScope_Input,
										 1,
										 &on,
										 sizeof(on)),
					"enabling input on HAL unit");
	
	UInt32 off = 0;
	OFXAU_RET_FALSE(AudioUnitSetProperty(*_unit,
										 kAudioOutputUnitProperty_EnableIO,
										 kAudioUnitScope_Output,
										 0,
										 &off,
										 sizeof(off)),
					"disabling output on HAL unit");
	
	AudioDeviceID inputDeviceID = kAudioObjectUnknown;
	UInt32 deviceIDSize = sizeof( AudioDeviceID );
	AudioObjectPropertyAddress prop_addr = {
		kAudioHardwarePropertyDefaultInputDevice,
		kAudioObjectPropertyScopeGlobal,
		kAudioObjectPropertyElementMaster
	};
	
	OFXAU_RET_FALSE(AudioObjectGetPropertyData(kAudioObjectSystemObject,
											   &prop_addr,
											   0,
											   NULL,
											   &deviceIDSize,
											   &inputDeviceID),
					"getting device ID for default input");
	
    if(deviceID != -1) inputDeviceID = deviceID;
    
	OFXAU_RET_FALSE(AudioUnitSetProperty(*_unit,
										 kAudioOutputUnitProperty_CurrentDevice,
										 kAudioUnitScope_Global,
										 0,
										 &inputDeviceID,
										 deviceIDSize),
					"setting HAL unit's device ID");
	
    cout<<"inputDeviceID "<<inputDeviceID<<endl;
    
	AudioStreamBasicDescription deviceASBD = {0};
	UInt32 ASBDSize = sizeof(deviceASBD);
	OFXAU_RET_FALSE(AudioUnitGetProperty(*_unit,
										 kAudioUnitProperty_StreamFormat,
										 kAudioUnitScope_Output,
										 1,
										 &deviceASBD,
										 &ASBDSize),
					"getting hardware stream format");
	
	deviceASBD.mSampleRate = 44100;
	
	OFXAU_RET_FALSE(AudioUnitSetProperty(*_unit,
										 kAudioUnitProperty_StreamFormat,
										 kAudioUnitScope_Output,
										 1,
										 &deviceASBD,
										 sizeof(deviceASBD)),
					"setting input sample rate to 44100");
	
	AURenderCallbackStruct inputCallback = {RenderCallback, &_impl->ctx};
	
	OFXAU_RET_FALSE(AudioUnitSetProperty(*_unit,
										 kAudioOutputUnitProperty_SetInputCallback,
										 kAudioUnitScope_Global,
										 0,
										 &inputCallback,
										 sizeof(inputCallback)),
					"setting hardware input callback");
	
	OFXAU_RET_BOOL(AudioUnitInitialize(*_unit),
				   "initializing hardware input unit after setting it to input mode");
}

#pragma mark - Callbacks / Rendering

// ----------------------------------------------------------
OSStatus ofxAudioUnitInput::render(AudioUnitRenderActionFlags *flags,
								   const AudioTimeStamp *timestamp,
								   UInt32 bus,
								   UInt32 frames,
								   AudioBufferList *data)
// ----------------------------------------------------------
{
	return PullCallback(&_impl->ctx, flags, timestamp, bus, frames, data);
}

// ----------------------------------------------------------
OSStatus RenderCallback(void *inRefCon,
						AudioUnitRenderActionFlags *ioActionFlags,
						const AudioTimeStamp *inTimeStamp,
						UInt32 inBusNumber,
						UInt32 inNumberFrames,
						AudioBufferList *ioData)
// ----------------------------------------------------------
{
	InputContext * ctx = static_cast<InputContext *>(inRefCon);
	
	OSStatus s = AudioUnitRender(*(ctx->inputUnit),
								 ioActionFlags,
								 inTimeStamp,
								 inBusNumber,
								 inNumberFrames,
								 ctx->bufferList.get());
	
	OFXAU_PRINT(s, "rendering audio input");
	
	if(s == noErr) {
		size_t buffersToCopy = min(ctx->bufferList->mNumberBuffers, ctx->circularBuffers.size());
		
		for(int i = 0; i < buffersToCopy; i++) {
			TPCircularBuffer * circBuffer = &ctx->circularBuffers[i];
			if(circBuffer) {
				TPCircularBufferProduceBytes(circBuffer,
											 ctx->bufferList->mBuffers[i].mData,
											 inNumberFrames * sizeof(AudioUnitSampleType));
			}
		}
	}
	
	return s;
}

// ----------------------------------------------------------
OSStatus PullCallback(void *inRefCon,
					  AudioUnitRenderActionFlags *ioActionFlags,
					  const AudioTimeStamp *inTimeStamp,
					  UInt32 inBusNumber,
					  UInt32 inNumberFrames,
					  AudioBufferList *ioData)
// ----------------------------------------------------------
{
	InputContext * ctx = static_cast<InputContext *>(inRefCon);
	
	size_t buffersToCopy = min(ioData->mNumberBuffers, ctx->circularBuffers.size());
	
	for(int i = 0; i < buffersToCopy; i++) {
		int32_t circBufferSize;
		AudioUnitSampleType * circBufferTail = (AudioUnitSampleType *) TPCircularBufferTail(&ctx->circularBuffers[i], &circBufferSize);
		bool circBufferHasEnoughSamples = circBufferSize / sizeof(AudioUnitSampleType) >= inNumberFrames ? true : false;
		
		if(!circBufferHasEnoughSamples) {
			// clear buffer, so bytes that don't get written are silence instead of noise
			memset(ioData->mBuffers[i].mData, 0, ioData->mBuffers[i].mDataByteSize);
		}
		
		size_t bytesToConsume = min(ioData->mBuffers[i].mDataByteSize, (UInt32)circBufferSize);
		memcpy(ioData->mBuffers[i].mData, circBufferTail, bytesToConsume);
		TPCircularBufferConsume(&ctx->circularBuffers[i], bytesToConsume);
	}
	
	return noErr;
}
