import smtplib
import datetime
from email.mime.text import MIMEText
from flask import current_app

def notify(notifyType, message, all=True):
    # Only notify if less than 3 notifications in the past 24 hours
    sendNotification = True
    now = datetime.datetime.now()
    if current_app.config.get(notifyType) is None:
        # Create and track this notify type
        current_app.config[notifyType] = (now, 1)

    else:
        oneDayAgo = now - datetime.timedelta(days=1)
        previousNotification = current_app.config.get(notifyType)

        if previousNotification[0] > oneDayAgo and previousNotification[1] >= 3:
            # If last notify was newer than 1 day ago and there have been 3 notifications
            sendNotification = False

        elif previousNotification[0] > oneDayAgo and previousNotification[1] < 3:
            # If last notify was newer than 1 day ago and there less than 3 notifications
            current_app.config[notifyType] = (
                now, previousNotification[1] + 1)

        else:
            # Last notification was more than 1 day ago start over
            current_app.config[notifyType] = (now, 1)

    if sendNotification:
        sender = current_app.config.get('SMTP_EMAIL')
        recipients = current_app.config.get('ALL_NOTIFY') if all else current_app.config.get('PRIMARY_NOTIFY')

        # Build email header
        msg = MIMEText(message)
        msg['Subject'] = 'Arduino Water Control Temperature Alert'
        msg['From'] = sender
        msg['To'] = ', '.join(recipients)

        server = smtplib.SMTP_SSL(
            current_app.config.get('SMTP_DOMAIN'), 
            port=current_app.config.get('SMTP_PORT'))

        server.login(sender, current_app.config.get('SMTP_PASSWORD'))
        server.sendmail(sender, recipients, msg.as_string())
        server.quit()
