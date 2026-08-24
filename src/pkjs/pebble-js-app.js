Pebble.addEventListener("ready",
  function(e) {
    // 1. Send the time instantly (your original code)
    var time = Math.round((new Date()).getTime() / 1000);
    Pebble.sendAppMessage({"0": time}, 
      function(e) { console.log('Time sent successfully!'); },
      function(e) { console.log('Error sending time!'); }
    );

    // 2. Now ask for the GPS location
    navigator.geolocation.getCurrentPosition(
      function(pos) {
        // Success! Build the location dictionary
        var locationDict = {
          "1": Math.round(pos.coords.latitude * 10000),
          "2": Math.round(pos.coords.longitude * 10000)
        };
        
        // Send the location to the watch
        Pebble.sendAppMessage(locationDict,
          function(e) { console.log('Location info sent to Pebble successfully!'); },
          function(e) { console.log('Error sending location info to Pebble!'); }
        );
      },
      function(err) { 
        console.log('Error requesting location: ' + err.message); 
      },
      { enableHighAccuracy: false, timeout: 15000, maximumAge: 60000 }
    );
  }
                       );