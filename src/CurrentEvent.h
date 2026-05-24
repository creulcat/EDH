#pragma once

#include <string>

class CurrentEvent {
public:
    // Cached from truck/job configuration events (available on all webhook events once known)
    std::string truck;
    std::string cargo;
    // Cached from job configuration (source/destination cities)
    std::string startingLocation;
    std::string destination;
    // job.cancelled
    std::string penalty;
    // job.delivered
    std::string revenue;
    std::string earnedXp;
    std::string cargoDamage;
    std::string distanceDriven;
    std::string deliveryTime;
    std::string autoparkUsed;
    std::string autoloadUsed;
    // player.fined
    std::string offence;
    std::string fineAmount;
    // player.tollgate
    std::string tollAmount;
    // player.use (ferry/train)
    std::string useAmount;
    std::string useStartingLocation;
    std::string useDestination;
};
