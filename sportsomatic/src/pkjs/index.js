var keys = require('message_keys');

var watchId = null;
var lastSend = 0;
var throttledMs = 1000;

var CMD = {
  START: 1,
  PAUSE: 2,
  RESUME: 3,
  STOP: 4
};

// Settings passthrough: when phone-side config is added, send them to watch.
function sendSettings(settings) {
  Pebble.sendAppMessage(settings, function() {}, function(err) {
    console.log('Settings send error: ' + JSON.stringify(err));
  });
}

function sendStatus(text) {
  Pebble.sendAppMessage(
    (function() {
      var msg = {};
      msg[keys.KEY_STATUS] = text;
      return msg;
    })(),
    function() {},
    function(err) {
      console.log('STATUS send error: ' + JSON.stringify(err));
    }
  );
}

function sendError(text) {
  Pebble.sendAppMessage(
    (function() {
      var msg = {};
      msg[keys.KEY_ERROR] = text;
      return msg;
    })(),
    function() {},
    function(err) {
      console.log('ERROR send error: ' + JSON.stringify(err));
    }
  );
}

function forwardPosition(pos) {
  var now = Date.now();
  if (now - lastSend < throttledMs) {
    return;
  }
  lastSend = now;

  var coords = pos.coords;
  var payload = {};
  payload[keys.KEY_LAT] = Math.round(coords.latitude * 1000000);
  payload[keys.KEY_LON] = Math.round(coords.longitude * 1000000);
  if (!isNaN(coords.speed)) {
    payload[keys.KEY_SPEED] = Math.round(coords.speed * 100); // cm/s precision
  }
  if (!isNaN(coords.accuracy)) {
    payload[keys.KEY_ACCURACY] = Math.round(coords.accuracy);
  }
  payload[keys.KEY_TIMESTAMP] = Math.round(pos.timestamp / 1000); // seconds

  Pebble.sendAppMessage(payload, function() {}, function(err) {
    console.log('Pos send failed: ' + JSON.stringify(err));
  });
}

function startGps() {
  if (watchId !== null) {
    navigator.geolocation.clearWatch(watchId);
    watchId = null;
  }

  if (!navigator.geolocation) {
    sendError('No geolocation');
    return;
  }

  watchId = navigator.geolocation.watchPosition(
    forwardPosition,
    function(err) {
      sendError('GPS err ' + err.code);
    },
    {
      enableHighAccuracy: true,
      maximumAge: 1000,
      timeout: 10000
    }
  );
  sendStatus('gps_on');
}

function pauseGps() {
  if (watchId !== null) {
    navigator.geolocation.clearWatch(watchId);
    watchId = null;
  }
  sendStatus('gps_paused');
}

function stopGps() {
  if (watchId !== null) {
    navigator.geolocation.clearWatch(watchId);
    watchId = null;
  }
  sendStatus('gps_off');
}

Pebble.addEventListener('ready', function() {
  console.log('JS ready');
  sendStatus('ready');
});

Pebble.addEventListener('message', function(evt) {
  var payload = evt.payload || {};
  var cmd = payload[keys.KEY_CMD];

  switch (cmd) {
    case CMD.START:
      startGps();
      break;
    case CMD.RESUME:
      startGps();
      break;
    case CMD.PAUSE:
      pauseGps();
      break;
    case CMD.STOP:
      stopGps();
      break;
    default:
      console.log('Unhandled cmd: ' + cmd);
      break;
  }

  // Forward summaries to phone logs for now
  if (payload[keys.KEY_SUMMARY]) {
    console.log('Run summary: ' + payload[keys.KEY_SUMMARY]);
  }

  // If the payload contains any settings keys (e.g., from a config page), forward as-is.
  var hasSettings = false;
  var settings = {};
  [keys.KEY_UNITS, keys.KEY_AUTO_LAP_ENABLED, keys.KEY_AUTO_LAP_DIST, keys.KEY_AUTO_RUN, keys.KEY_AUTO_STOP, keys.KEY_DISTANCE_ALERTS, keys.KEY_SPORT, keys.KEY_POOL_LENGTH].forEach(function(k) {
    if (payload[k] !== undefined) {
      hasSettings = true;
      settings[k] = payload[k];
    }
  });
  if (hasSettings) {
    sendSettings(settings);
  }
});
