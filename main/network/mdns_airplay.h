#pragma once

/**
 * Initialize mDNS and advertise AirPlay 2 services
 *
 * This publishes:
 * - _airplay._tcp service (AirPlay 2)
 * - _raop._tcp service (Remote Audio Output Protocol)
 *
 * With all required TXT records for iOS to recognize the device
 */
void mdns_airplay_init(void);

/**
 * Remove AirPlay/RAOP service advertisements without shutting down mDNS.
 *
 * mDNS itself stays alive because DACP remote-control discovery also uses it.
 */
void mdns_airplay_deinit(void);
