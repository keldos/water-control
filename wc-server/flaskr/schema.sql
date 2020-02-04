-- initialize the database.
-- drop any existing data and create empty tables.

drop table if exists weather;
drop table if exists arduino_log;
drop table if exists arduino_settings;

create table weather (
  time datetime not null,
  type text not null,
  value float not null,
  primary key (time, type)
);

create table arduino_log (
  timestamp timestamp not null default current_timestamp,
  reading text not null,
  value float not null,
  primary key (timestamp, reading)
);

create table arduino_settings (
  override_time text not null default 'N',
  water_enabled text not null default 'N',
  hour integer not null,
  minute integer not null,
  second integer not null,
  min_soil integer not null,
  run_duration integer not null,
  run_times text not null,
  max_chance integer not null,
  total_accumulation real not null
);

-- Default settings
insert into arduino_settings values (
'N', 'N', 0, 0, 0, 250, 15, '[7,10,13]', 70, 3.0
);