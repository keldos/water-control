import atexit, json, re
import urllib.request
import dateutil.parser
from flask import current_app, g
from flaskr.db import get_db
from flaskr.notify import notify
from apscheduler.schedulers.background import BackgroundScheduler
from dateutil.relativedelta import relativedelta

def insert_data_for(db, data, property):
  # Insert weather data splitting up grouped durations
  for item in data[property]['values']:
    hours = 1
    dateParts = item['validTime'].split('/')
    dateTime = dateutil.parser.parse(dateParts[0])

    # Extract duration
    durationReg = re.compile('^PT(\d+)H$')
    match = re.match(durationReg, dateParts[1])
    if match:
      hours = int(match.group(1))

    for i in range(0, hours):
      hourDate = dateutil.parser.parse(dateParts[0]) + relativedelta(hours=+i)
      db.execute(
        'insert or replace into weather (time, type, value) values (?, ?, ?)',
        (str(hourDate), property, item['value'])
      )

      # Clear old data
      db.execute(
        'delete from weather where time < datetime("now", "localtime", "-30 days")'
      )

def start_weather_service(app):
  def get_weather():
    with app.app_context():
      db = get_db()

      # Get grid point URL from coordinates
      req = urllib.request.Request(
        current_app.config.get('WEATHER_API'), 
        data=None,
        headers=current_app.config.get('HEADERS')
      )
      response = urllib.request.urlopen(req)
      str_response = response.read().decode('utf-8')
      data = json.loads(str_response)
      gridURL = data['forecastGridData']

      # Get hourly data from grid point
      req = urllib.request.Request(
        gridURL,
        data=None,
        headers=current_app.config.get('HEADERS')
      )
      response = urllib.request.urlopen(req)
      str_response = response.read().decode('utf-8')
      data = json.loads(str_response)

      # Insert the fresh data
      insert_data_for(db, data, 'probabilityOfPrecipitation')
      insert_data_for(db, data, 'quantitativePrecipitation')
      insert_data_for(db, data, 'minTemperature')
      insert_data_for(db, data, 'temperature')

      # Save database changes
      db.commit()

      # Check temperatures for < 0c and send alerts
      event = db.execute(
          '''select time, value
             from weather
             where type in ('minTemperature', 'temperature')
               and time > datetime('now', 'localtime', '-1 hour')
               and value < 0.0
             order by time asc
          '''
      ).fetchone()

      # Only send alerts if the water is enabled
      waterEnabled = db.execute(
          '''select water_enabled
             from arduino_settings
             where water_enabled = 'Y'
          '''
      ).fetchone()

      if event is not None and waterEnabled is not None:
        notify('freeze', '''Warning: Below freezing temperature (%s C) at %s. 
          Turn off the water and drain the system!''' % 
          (round(event['value'], 2), event['time']), all=True)

  # Start the scheduler to download data every 30 minutes
  sched = BackgroundScheduler(daemon=True)
  sched.add_job(get_weather, 'interval', minutes=30)
  sched.start()

  atexit.register(lambda: sched.shutdown(wait=False))

  # Get the weather immediately
  get_weather()
