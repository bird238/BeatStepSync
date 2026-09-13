#include "plugin.hpp"

Plugin* pluginInstance = nullptr;

extern "C" void init(Plugin* p) {
    pluginInstance = p;
    p->addModel(modelBeatStepSeq);
}
