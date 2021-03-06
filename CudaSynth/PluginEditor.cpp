#include "PluginProcessor.h"
#include "PluginEditor.h"

//==============================================================================
PluginEditor::PluginEditor(PluginProcessor& owner)
	: AudioProcessorEditor(owner),
	  tooltipWindow(this, 1500),
      midiKeyboard (owner.keyboardState, MidiKeyboardComponent::horizontalKeyboard),
	  partialLevelsComponent(this, parameterStates.partialLevels),
	  volumeADSR(this, parameterStates.volumeEnvelope.getAdsr(), "Volume", ADSREditor::ClassicKnobs, ADSREditor::NormalizedDepthLimits),
	  volumeLFOFreq(this, parameterStates.volumeEnvelope.getLfo()->getFreqAdsr(), "LFO Freq", ADSREditor::AsrWithPeaksKnobs, ADSREditor::LFOFrequencyLimits),
	  volumeLFODepth(this, parameterStates.volumeEnvelope.getLfo()->getDepthAdsr(), "LFO Depth", ADSREditor::AsrWithPeaksKnobs, ADSREditor::NormalizedDepthLimitsPlusOrMinus),
	  stereoADSR(this, parameterStates.stereoPanEnvelope.getAdsr(), "Stereo Pan", ADSREditor::ClassicKnobsWithScaleByIdx, ADSREditor::NormalizedDepthLimitsPlusOrMinus),
	  stereoLFOFreq(this, parameterStates.stereoPanEnvelope.getLfo()->getFreqAdsr(), "LFO Freq", ADSREditor::AsrWithPeaksKnobs, ADSREditor::LFOFrequencyLimits),
	  stereoLFODepth(this, parameterStates.stereoPanEnvelope.getLfo()->getDepthAdsr(), "LFO Depth", ADSREditor::AsrWithPeaksKnobs, ADSREditor::NormalizedDepthLimitsPlusOrMinus),
	  detuneRandEditor(this, &parameterStates.detuneEnvelope, "Rand Detune"),
	  detuneADSR(this, parameterStates.detuneEnvelope.getAdsrLfo()->getAdsr(), "Detune", ADSREditor::ClassicKnobsWithScaleByIdx, ADSREditor::NormalizedDepthLimits),
	  detuneLFOFreq(this, parameterStates.detuneEnvelope.getAdsrLfo()->getLfo()->getFreqAdsr(), "LFO Freq", ADSREditor::AsrWithPeaksKnobs, ADSREditor::LFOFrequencyLimits),
	  detuneLFODepth(this, parameterStates.detuneEnvelope.getAdsrLfo()->getLfo()->getDepthAdsr(), "LFO Depth", ADSREditor::AsrWithPeaksKnobs, ADSREditor::NormalizedDepthLimitsPlusOrMinus),
	  delaySpaceADSR(this, parameterStates.delayEnvelope.getSpaceBetweenEchoes()->getAdsr(), "Delay Time", ADSREditor::ClassicKnobsWithScaleByIdx, ADSREditor::NormalizedDepthLimits),
	  delayAmpLossADSR(this, parameterStates.delayEnvelope.getAmplitudeLostPerEcho()->getAdsr(), "Amp Loss Per Echo", ADSREditor::ClassicKnobsWithScaleByIdx, ADSREditor::NormalizedDepthLimits),
	  filterComponent(this, parameterStates.filterEnvelope.getShape()),
	  filterADSR(this, parameterStates.filterEnvelope.getShift(), "Transpose", ADSREditor::ClassicKnobsWithPeakNoShiftByIdx, ADSREditor::FreqFilterDepthLimits)
{
	// add the parameter editors
	addAndMakeVisible(partialLevelsComponent);
	addAndMakeVisible(volumeADSR);
	addAndMakeVisible(volumeLFOFreq);
	addAndMakeVisible(volumeLFODepth);
	addAndMakeVisible(stereoADSR);
	addAndMakeVisible(stereoLFOFreq);
	addAndMakeVisible(stereoLFODepth);
	addAndMakeVisible(detuneRandEditor);
	addAndMakeVisible(detuneADSR);
	addAndMakeVisible(detuneLFOFreq);
	addAndMakeVisible(detuneLFODepth);
	addAndMakeVisible(delaySpaceADSR);
	addAndMakeVisible(delayAmpLossADSR);
	addAndMakeVisible(filterComponent);
	addAndMakeVisible(filterADSR);

    // add the midi keyboard component
    addAndMakeVisible (midiKeyboard);

    // add the triangular resizer component for the bottom-right of the UI
    addAndMakeVisible (resizer = new ResizableCornerComponent (this, &resizeLimits));
    resizeLimits.setSizeLimits (150, 150, 800, 300);

    // set our component's initial size to be the last one that was stored in the filter's settings
    setSize (owner.lastUIWidth,
             owner.lastUIHeight);

	// initialize synth parameters on device side
	parametersChanged();
}

PluginEditor::~PluginEditor()
{
}

void PluginEditor::paint (Graphics& g)
{
	// paint a solid background
	g.setColour(Colour(0x30, 0x30, 0x30));
    g.fillAll();
}

void PluginEditor::resized()
{
    const int keyboardHeight = 70;

	partialLevelsComponent.setBounds(partialLevelsComponent.getLocalBounds().translated(4, 0));

	// size the filter
	float filterWidth = 256;
	float filterHeight = 128;
	filterComponent.setBounds(0, 0, filterWidth, filterHeight);

	// position the ADSR editors
	int padding = 6;
	Component* volumeEditors[] = { &volumeADSR, &volumeLFOFreq, &volumeLFODepth, NULL };
	Component* stereoEditors[] = { &stereoADSR, &stereoLFOFreq, &stereoLFODepth, NULL };
	Component* detuneEditors[] = { &detuneRandEditor, &detuneADSR, &detuneLFOFreq, &detuneLFODepth, NULL };
	Component* delayEditors[] = { &delaySpaceADSR, &delayAmpLossADSR, NULL };
	Component* filterEditors[] = { &filterComponent, &filterADSR, NULL };
	Point<int> editorStartPoints[] = { Point<int>(partialLevelsComponent.getWidth() + 16, 30), Point<int>(4, 120), Point<int>(4, 210), Point<int>(4, 300), Point<int>(4, 390) };
	Component** editors[] = { volumeEditors, stereoEditors, detuneEditors, delayEditors, filterEditors };
	int numEditors = sizeof(editors) / sizeof(ADSREditor**);
	for (int row = 0; row < numEditors; ++row) {
		Point<int> curPos = editorStartPoints[row];
		for (int i = 0; editors[row][i] != NULL; ++i) {
			Component *cur = editors[row][i];
			Rectangle<int> curBounds = cur->getLocalBounds();
			curBounds.setPosition(curPos);
			cur->setBounds(curBounds);
			curPos = cur->getBounds().getTopRight().translated(padding, 0);
		}
	}

	// position the keyboard
	midiKeyboard.setBounds(4, getHeight() - keyboardHeight - 4, getWidth() - 8, keyboardHeight);
	// add a resizer element to bottom-right.
    resizer->setBounds (getWidth() - 16, getHeight() - 16, 16, 16);

    getProcessor().lastUIWidth = getWidth();
    getProcessor().lastUIHeight = getHeight();
}

void PluginEditor::parametersChanged() {
	kernel::parameterStatesChanged(&parameterStates);
}