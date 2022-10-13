# windows-process-audio-capture
+ process audio capture using wasapi hook
+ hook procedure and audio mix code was copied from obs-sudio project
# Fetures
+ support multi IAudioClient instance and audio mix
+ only hook IAudioRenderClient ReleaseBuffer, support capture audio after IAudioClient initialize called
+ support Win7+
# How to use
+ Copy data and obs-plugins folder into your obs-studio directory
+ Launch OBS and add wasapi capture source in your scene
+ Select process which you want to capture
# How to build
+ Clone [obs-studio repository](https://github.com/obsproject/obs-studio) first
+ Clone this repository into the obs-studio/plugins directory
+ Open obs-studio/plugins/CMakeLists.txt, add `add_subdirectory(wasapi-capture)` after `if(OS_WINDOWS)`
+ follow obs-studio build instructions to build the project
# License
MIT
