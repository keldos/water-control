from flask import (
  Blueprint, flash, g, redirect, render_template, request, url_for, Response
)

from flaskr.db import get_db
import datetime, json, os, base64

bp = Blueprint('weather', __name__)


@bp.route('/')
def index():
  # Display the index
  return render_template('weather/index.html')

@bp.route('/weather/', methods=('GET', 'POST'))
def weather():
  # Show all weather data
  title = "Weather Data"
  sqlStatement = '''select time, type, value
                    from weather
                    where time > datetime('now', 'localtime', '-1 hour')
                    order by time asc
                    limit ?
                 '''
  return list_page(title, sqlStatement)


@bp.route('/weather/arduino-log/', methods=('GET', 'POST'))
def arduino_log():
  # Show arduino log data
  title = "Arduino Log"
  sqlStatement = '''select datetime(timestamp, 'localtime') as timestamp, reading, value
                    from arduino_log
                    order by timestamp desc, reading asc
                    limit ?
                 '''
  return list_page(title, sqlStatement)


def list_page(title, sqlStatement):
  # Render a dynamic list page
  defaultNumRows = 100
  numRows = defaultNumRows
  if request.method == 'POST':
    numRows = request.form['num_rows']
    if not numRows.isdigit():
      numRows = defaultNumRows
      flash('Value must be numeric.')

  db = get_db()
  rows = db.execute(sqlStatement, (numRows,)).fetchall()

  return render_template('/weather/list-page.html', title=title, 
            colNames=rows[0].keys(), rows=rows, numRows=numRows)


@bp.route('/weather/stats/arduino-log.json')
def arduino_log_json():
  # Show all arduino log data as json
  db = get_db()
  rows = db.execute(
    '''select datetime(timestamp, 'localtime') as timestamp, reading, value
       from arduino_log
    '''
  ).fetchall()
  return Response(json.dumps([dict(row) for row in rows]), mimetype='text/json')


@bp.route('/weather/stats/')
def stats():
  # Display statistics page with graph
  return render_template('weather/stats.html')


@bp.route('/weather/arduino-settings/',  methods=('GET', 'POST'))
def arduino_settings():
  # Configure settings for Arduino water control
  db = get_db()
  if request.method == 'POST':
    overrideTime = request.form.get('override_time') != None
    waterEnabled = request.form.get('water_enabled') != None
    hour = request.form['hour']
    minute = request.form['minute']
    second = request.form['second']
    minSoil = request.form['min_soil']
    runDuration = request.form['run_duration']
    runTimes = request.form['run_times']
    maxChance = request.form['max_chance']
    totalAccumulation = request.form['total_accumulation']

    for key in request.form:
      print(key, request.form[key])

    error = None

    if (not hour or 
        not minute or 
        not second or 
        not minSoil or 
        not runDuration or 
        not runTimes or 
        not maxChance or 
        not totalAccumulation):
      error = 'All fields are required.'

    if error is None:
      db.execute(
        '''update arduino_settings 
           set override_time = ?,
               water_enabled = ?,
               hour = ?,
               minute = ?,
               second = ?,
               min_soil = ?,
               run_duration = ?,
               run_times = ?,
               max_chance = ?,
               total_accumulation = ?
        ''',
        ('Y' if overrideTime else 'N', 'Y' if waterEnabled else 'N',
         hour, minute, second, minSoil, runDuration, 
         runTimes, maxChance, totalAccumulation)
      )
      db.commit()
    else:
      flash(error)

  row = db.execute(
    "select * from arduino_settings limit 1"
  ).fetchone()
  return render_template('weather/arduino-settings.html', row=row)


@bp.route('/weather/log-data/', methods=('GET', 'POST'))
def log_data():
  db = get_db()
  if request.method == 'POST':
    # Decode base64 and convert to ascii string
    data = request.form.get('data')
    dataBytes = data.encode('ascii')
    jsonBytes = base64.b64decode(dataBytes)
    jsonString = jsonBytes.decode('ascii')

    # Trim everything after null terminator
    jsonString = jsonString.split('\0', 1)[0]

    # Parse JSON string into object
    jsonObject = json.loads(jsonString)

    # Loop through all readings in JSON object and store them in the 
    # arduino_log table
    for key in jsonObject:
      db.execute(
        'insert into arduino_log(reading, value) values (?, ?)',
        (key, jsonObject[key])
      )
      db.commit()
  
  return ''


@bp.route('/weather/get-settings/')
def get_settings():
  db = get_db()
  settingsRow = db.execute(
    '''select
          override_time,
          water_enabled,
          hour,
          minute,
          second,
          min_soil,
          run_duration,
          run_times
       from arduino_settings
       limit 1
    '''
  ).fetchone()

  settingsDict = dict(settingsRow)
  overrideTime = settingsDict.pop('override_time')
  settingsDict['water_enabled'] = True if (settingsDict['water_enabled'] == 'Y') else False
  settingsDict['run_times'] = json.loads(settingsDict['run_times'])

  now = datetime.datetime.now()
  settingsDict['year'] = int(str(now.year)[-2:])
  settingsDict['month'] = now.month
  settingsDict['day'] = now.day

  if overrideTime != 'Y':
    settingsDict['hour'] = now.hour
    settingsDict['minute'] = now.minute
    settingsDict['second'] = now.second

  settingsDict['run_water'] = run_water()

  jsonString = json.dumps(settingsDict, separators=(',', ':'))

  return Response(jsonString, mimetype='text/json')


def run_water():
  # Use NWS weather data to determine if the water should run
  #   result == True - run water
  #   result == False - skip water
  result = True
  db = get_db()
  
  # Get maximum chance of percipitation in the next 3 hours
  rainChance = db.execute(
    '''select max(value) as max_chance
       from weather
       where time > datetime('now', 'localtime')
         and time < datetime('now', 'localtime', '+4 hours')
         and type = 'probabilityOfPrecipitation'
       group by type
    '''
  ).fetchone()

  # Get total accumulation over the next 3 hours
  rainAccumulation = db.execute(
    '''select sum(value) as total_accumulation
       from weather
       where time > datetime('now', 'localtime')
         and time < datetime('now', 'localtime', '+4 hours')
         and type = 'quantitativePrecipitation'
       group by type
    '''
  ).fetchone()

  # Check if Arduino has completed a water cycle today
  endingFound = db.execute(
    '''select 1 as ending
       from arduino_log
       where datetime(timestamp, 'localtime') >= date('now','localtime','start of day')
         and reading = 'ending'
    '''
  ).fetchone()

  settings = db.execute(
    "select * from arduino_settings limit 1"
  ).fetchone()

  # Display yes/no based on rain chance and accumulation
  if rainChance is None or rainAccumulation is None:
    print('Error: missing forcast data.')
    result = True
  else:
    if(rainChance['max_chance'] > settings['max_chance'] and
       rainAccumulation['total_accumulation'] > settings['total_accumulation']):
      # High chance of rain with plenty of accumulation so don't run water
      result = False
    else:
      # Not enough rain so run water if it hasn't run already
      if endingFound is None:
        result = True
      else:
        result = False

  return result
