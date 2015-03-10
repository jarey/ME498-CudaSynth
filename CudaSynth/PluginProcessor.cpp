/*
  ==============================================================================

    This file was auto-generated by the Jucer!

    It contains the basic startup code for a Juce application.

  ==============================================================================
*/

#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "kernel.h"
#include "defines.h"

#ifndef PI
	#define PI 3.14159265358979323846
#endif

AudioProcessor* JUCE_CALLTYPE createPluginFilter();


class AdditiveSynthSound : public SynthesiserSound
{
public:
    AdditiveSynthSound() {}

    bool appliesToNote (int /*midiNoteNumber*/) override  { return true; }
    bool appliesToChannel (int /*midiChannel*/) override  { return true; }
};

//==============================================================================
/** A simple demo synth voice that just plays a sine wave.. */
class AdditiveSynthVoice  : public SynthesiserVoice
{
	// this acts as an ID to associate this voice with the resources on the GPU side.
	unsigned myVoiceNumber;
	//we use a double-buffering strategy to allow bufferDrain to be drained into the audio output
	//  while bufferFill is being filled in in a different thread.
	//Once bufferDrain is fully drained, it waits for bufferFill to be filled and then swaps the pointers:
	//  bufferDrain* with bufferFill*, which point to either one of the underlying bufferA, bufferB buffers.
	float bufferA[BUFFER_BLOCK_SIZE*NUM_CH], bufferB[BUFFER_BLOCK_SIZE*NUM_CH];
	// flag to kill the worker thread (done upon destruction of the Voice @ program shutdown)
	std::atomic<bool> isAlive;
	// pass on to the synth kernel that the note is in release mode (ADSR)
	std::atomic<bool> wasNoteReleased;
	std::atomic<float> fundamentalFreq;
	unsigned int sampleIdx;
	std::mutex bufferBMutex;
	bool needFillBuffer;
	std::condition_variable needFillBufferCV;
	std::thread fillThread;
public:
	AdditiveSynthVoice(unsigned voiceNum) : myVoiceNumber(voiceNum),
		isAlive(true), wasNoteReleased(false), fundamentalFreq(0), sampleIdx(0),
		needFillBuffer(false),
		fillThread([](AdditiveSynthVoice *v) { v->fillLoop(); }, this) {
		memset(bufferA, 0, BUFFER_BLOCK_SIZE*NUM_CH*sizeof(float));
		memset(bufferB, 0, BUFFER_BLOCK_SIZE*NUM_CH*sizeof(float));
	}
	~AdditiveSynthVoice() {
		//signal fillThread to exit
		isAlive.store(false);
		needFillBufferCV.notify_one();
		fillThread.join();
	}

    bool canPlaySound (SynthesiserSound* sound) override
    {
		return dynamic_cast<AdditiveSynthSound*> (sound) != nullptr;
    }

    void startNote (int midiNoteNumber, float velocity,
                    SynthesiserSound* /*sound*/,
                    int /*currentPitchWheelPosition*/) override
    {
		std::unique_lock<std::mutex> lock(bufferBMutex);
		memset(bufferA, 0, sizeof(bufferB));
		memset(bufferB, 0, sizeof(bufferB));

		sampleIdx = BUFFER_BLOCK_SIZE; // trigger a re-render of the current block
		wasNoteReleased = false;

		double cyclesPerSecond = MidiMessage::getMidiNoteInHertz(midiNoteNumber);
		assert(getSampleRate() == SAMPLE_RATE);
		fundamentalFreq = cyclesPerSecond * 2*PI;
		kernel::onNoteStart(myVoiceNumber);
    }

    void stopNote (float /*velocity*/, bool allowTailOff) override
    {
		printf("stopNote() called on voice %i. allowTailOff? %i\n", myVoiceNumber, allowTailOff);
		wasNoteReleased = true;
		if (!allowTailOff) {
			// if we aren't allowed to do the release phase, end the note immediately.
			clearCurrentNote();
		}
    }

    void pitchWheelMoved (int /*newValue*/) override
    {
        // can't be bothered implementing this for the demo!
    }

    void controllerMoved (int /*controllerNumber*/, int /*newValue*/) override
    {
        // not interested in controllers in this case.
    }

    void renderNextBlock (AudioSampleBuffer& outputBuffer, int startSample, int numSamples) override
    {
		if (!isVoiceActive()) {
			return;
		}
		for (int localIdx = startSample; localIdx < startSample + numSamples; ++localIdx) {
			if (sampleIdx == BUFFER_BLOCK_SIZE) {
				sampleIdx = 0;
				waitAndSwapBuffers();
			} else if (sampleIdx == BUFFER_BLOCK_SIZE - 1 && std::isnan(bufferA[(BUFFER_BLOCK_SIZE - 1) * NUM_CH])) {
				// NaN at last buffer point signals end of note.
				printf("ending note from within renderNextBlock callback\n");
				bufferA[(BUFFER_BLOCK_SIZE - 1) * NUM_CH] = 0;
				clearCurrentNote();
				return;
			}
			for (int ch = outputBuffer.getNumChannels(); --ch >= 0;) {
				outputBuffer.addSample(ch, localIdx, bufferA[sampleIdx * NUM_CH + ch]);
			}
			++sampleIdx;
		}
    }
	void waitAndSwapBuffers() {
		//acquire lock on buffer B:
		std::unique_lock<std::mutex> lock(bufferBMutex);
		//copy buffer B into buffer A:
		memcpy(bufferA, bufferB, BUFFER_BLOCK_SIZE*NUM_CH*sizeof(float));

		//release buffer B lock and notify the filler thread.
		needFillBuffer = true;
		needFillBufferCV.notify_one();
	}

	void fillLoop() {
		unsigned baseIdx = 0;
		while (1) {
			// get access to buffer B
			std::unique_lock<std::mutex> lock(bufferBMutex);
			// wait until we are asked to fill the buffer, and then clear the flag
			needFillBufferCV.wait(lock, [this]() { return this->needFillBuffer || !this->isAlive; });
			if (!isAlive) {
				return;
			}
			this->needFillBuffer = false;

			// fill the buffer
			//printf("Was note released?: %i\n", wasNoteReleased.load());
			evaluateSynthVoiceBlock(bufferB, myVoiceNumber, baseIdx, fundamentalFreq, wasNoteReleased);
			//printf("last value in buffer: %f\n", bufferB[(BUFFER_BLOCK_SIZE - 1) * NUM_CH]);
			baseIdx += BUFFER_BLOCK_SIZE;
		}
	}
};

const float defaultGain = 1.0f;
const float defaultDelay = 0.5f;

//==============================================================================
PluginProcessor::PluginProcessor()
    : delayBuffer (2, 12000)
{
	File logfile = File::getCurrentWorkingDirectory().getChildFile("CUDASynth.log");
	fileLogger = new FileLogger(logfile, "Juce VST starting", 0);
	Logger::setCurrentLogger(fileLogger);
    // Set up some default values..
    gain = defaultGain;
    delay = defaultDelay;

    lastUIWidth = 400;
    lastUIHeight = 200;

    lastPosInfo.resetToDefault();
    delayPosition = 0;

    // Initialise the synth...
	// At runtime, each note gets assigned to a voice,
	// so we must create N voices to achieve a polyphony of N.
	for (int i = MAX_SIMULTANEOUS_SYNTH_NOTES; --i >= 0;)
		synth.addVoice(new AdditiveSynthVoice(i));
	synth.addSound(new AdditiveSynthSound());
}

PluginProcessor::~PluginProcessor()
{
	if (fileLogger) {
		Logger::setCurrentLogger(nullptr);
		delete fileLogger;
		fileLogger = NULL;
	}
}

//==============================================================================
int PluginProcessor::getNumParameters()
{
    return totalNumParams;
}

float PluginProcessor::getParameter(int index)
{
    // This method will be called by the host, probably on the audio thread, so
    // it's absolutely time-critical. Don't use critical sections or anything
    // UI-related, or anything at all that may block in any way!
    switch (index)
    {
        case gainParam:     return gain;
        case delayParam:    return delay;
        default:            return 0.0f;
    }
}

void PluginProcessor::setParameter(int index, float newValue)
{
    // This method will be called by the host, probably on the audio thread, so
    // it's absolutely time-critical. Don't use critical sections or anything
    // UI-related, or anything at all that may block in any way!
    switch (index)
    {
        case gainParam:     gain = newValue;  break;
        case delayParam:    delay = newValue;  break;
        default:            break;
    }
}

float PluginProcessor::getParameterDefaultValue(int index)
{
    switch (index)
    {
        case gainParam:     return defaultGain;
        case delayParam:    return defaultDelay;
        default:            break;
    }

    return 0.0f;
}

const String PluginProcessor::getParameterName(int index)
{
    switch (index)
    {
        case gainParam:     return "gain";
        case delayParam:    return "delay";
        default:            break;
    }

    return String::empty;
}

const String PluginProcessor::getParameterText(int index)
{
    return String (getParameter (index), 2);
}

//==============================================================================
void PluginProcessor::prepareToPlay(double sampleRate, int /*samplesPerBlock*/)
{
    // Use this method as the place to do any pre-playback
    // initialisation that you need..
    synth.setCurrentPlaybackSampleRate (sampleRate);
    keyboardState.reset();
    delayBuffer.clear();
}

void PluginProcessor::releaseResources()
{
    // When playback stops, you can use this as an opportunity to free up any
    // spare memory, etc.
    keyboardState.reset();
}

void PluginProcessor::reset()
{
    // Use this method as the place to clear any delay lines, buffers, etc, as it
    // means there's been a break in the audio's continuity.
    delayBuffer.clear();
}

void PluginProcessor::processBlock(AudioSampleBuffer& buffer, MidiBuffer& midiMessages)
{
    const int numSamples = buffer.getNumSamples();
	
    // Now pass any incoming midi messages to our keyboard state object, and let it
    // add messages to the buffer if the user is clicking on the on-screen keys
    keyboardState.processNextMidiBuffer (midiMessages, 0, numSamples, true);

    // and now get the synth to process these midi events and generate its output.
    synth.renderNextBlock (buffer, midiMessages, 0, numSamples);

    // In case we have more outputs than inputs, we'll clear any output
    // channels that didn't contain input data, (because these aren't
    // guaranteed to be empty - they may contain garbage).
    // for (int i = getNumInputChannels(); i < getNumOutputChannels(); ++i)
    //    buffer.clear (i, 0, buffer.getNumSamples());

    // ask the host for the current time so we can display it...
    AudioPlayHead::CurrentPositionInfo newTime;

    if (getPlayHead() != nullptr && getPlayHead()->getCurrentPosition (newTime))
    {
        // Successfully got the current time from the host..
        lastPosInfo = newTime;
    }
    else
    {
        // If the host fails to fill-in the current time, we'll just clear it to a default..
        lastPosInfo.resetToDefault();
    }
}

//==============================================================================
AudioProcessorEditor* PluginProcessor::createEditor()
{
	return new PluginEditor(*this);
}

//==============================================================================
void PluginProcessor::getStateInformation(MemoryBlock& destData)
{
    // You should use this method to store your parameters in the memory block.
    // Here's an example of how you can use XML to make it easy and more robust:

    // Create an outer XML element..
    XmlElement xml ("MYPLUGINSETTINGS");

    // add some attributes to it..
    xml.setAttribute ("uiWidth", lastUIWidth);
    xml.setAttribute ("uiHeight", lastUIHeight);
    xml.setAttribute ("gain", gain);
    xml.setAttribute ("delay", delay);

    // then use this helper function to stuff it into the binary blob and return it..
    copyXmlToBinary (xml, destData);
}

void PluginProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    // You should use this method to restore your parameters from this memory block,
    // whose contents will have been created by the getStateInformation() call.

    // This getXmlFromBinary() helper function retrieves our XML from the binary blob..
    ScopedPointer<XmlElement> xmlState (getXmlFromBinary (data, sizeInBytes));

    if (xmlState != nullptr)
    {
        // make sure that it's actually our type of XML object..
        if (xmlState->hasTagName ("MYPLUGINSETTINGS"))
        {
            // ok, now pull out our parameters..
            lastUIWidth  = xmlState->getIntAttribute ("uiWidth", lastUIWidth);
            lastUIHeight = xmlState->getIntAttribute ("uiHeight", lastUIHeight);

            gain  = (float) xmlState->getDoubleAttribute ("gain", gain);
            delay = (float) xmlState->getDoubleAttribute ("delay", delay);
        }
    }
}

const String PluginProcessor::getInputChannelName(const int channelIndex) const
{
    return String (channelIndex + 1);
}

const String PluginProcessor::getOutputChannelName(const int channelIndex) const
{
    return String (channelIndex + 1);
}

bool PluginProcessor::isInputChannelStereoPair(int /*index*/) const
{
    return true;
}

bool PluginProcessor::isOutputChannelStereoPair(int /*index*/) const
{
    return true;
}

bool PluginProcessor::acceptsMidi() const
{
   #if JucePlugin_WantsMidiInput
    return true;
   #else
    return false;
   #endif
}

bool PluginProcessor::producesMidi() const
{
   #if JucePlugin_ProducesMidiOutput
    return true;
   #else
    return false;
   #endif
}

bool PluginProcessor::silenceInProducesSilenceOut() const
{
    return false;
}

double PluginProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

//==============================================================================
// This creates new instances of the plugin..
AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
	return new PluginProcessor();
}