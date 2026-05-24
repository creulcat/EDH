#pragma once

#include "GameEvent.h"

#include <string>

class WebhookConfiguration {
public:
    std::string webhookURL;
    std::string payloadJSON;
    GameEvent eventTrigger;
};
