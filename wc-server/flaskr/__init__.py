import os
from flask import Flask

def create_app(test_config=None):
    # Create and configure an instance of the Flask application
    app = Flask(__name__, instance_relative_config=True)
    app.config.from_mapping(
        # A default secret that should be overridden by instance config
        SECRET_KEY='dev',
        # Store the database in the instance folder
        DATABASE=os.path.join(app.instance_path, 'flaskr.sqlite'),
    )

    if test_config is None:
        # Load the instance config, if it exists, when not testing
        app.config.from_pyfile('config.py', silent=False)
    else:
        # Load the test config if passed in
        app.config.update(test_config)

    # Ensure the instance folder exists
    try:
        os.makedirs(app.instance_path)
    except OSError:
        pass

    # Register the database commands
    from flaskr import db
    db.init_app(app)

    # Main weather server and configuration routes
    from flaskr import weather
    app.register_blueprint(weather.bp)

    # Pulls data from NWS on a schedule
    from flaskr import weatherService
    weatherService.start_weather_service(app)    

    # Set up index on the root path
    app.add_url_rule('/', endpoint='index')

    return app
