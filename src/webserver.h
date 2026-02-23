#ifndef WEBSERVER_H
#define WEBSERVER_H

#include "esp_err.h"

/**
 * Start the HTTP server on port 80.
 * Routes:
 *   GET /         - serve web interface
 *   GET /capture  - capture frame with ?method=X&quality=Y, return BMP
 */
esp_err_t webserver_start(void);

#endif /* WEBSERVER_H */
