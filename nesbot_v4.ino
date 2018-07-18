/*
 * NESBot version 4 by Mike Arnos
 * 
 */

#include <avr/pgmspace.h>
#include <EEPROM.h>

/****SD Card****/
#include <SdFat.h>
SdFat sd;
SdFile movie_file;
const uint8_t chipSelect = 14;    // A0 on Arduino Nano
/***************/

#define RIGHT 10
#define LEFT   9
#define DOWN   8
#define UP     7
#define START  6
#define SELECT 5
#define B      4
#define A      3

#define LATCH_IN  2   // can use digital pin 2 or 3 only
#define GREEN_LED 15  // analog pin 1
#define WHITE_LED 16  // analog pin 2
#define SWITCH    17  // analog pin 3

#define smbdh          0
#define smb3           1
#define blastermaster  2
#define radracer       3
#define metroid        4
#define drmario        5

unsigned long movie_length;
unsigned long frame = 0, file_loc = 0;         // This is the frame for which data is currently loaded
unsigned char next_buttons = 0, prev_buttons;  // This will fill the blank_pre_frames length
bool lag_frame, SD_buffer_full, first_latch = 0;
bool game_detect = 0;
unsigned int lag_frames, latch_timeout, lag_timeout;
volatile unsigned int frame_length, frame_total;
unsigned char SD_buffer[5], SD_buffer_pos;
unsigned char gameDetectLagCount = 0, total_game_movies = 0;
unsigned char current_game = 0, current_movie = 0;
char filename[256];

// 13 bytes to hold the tab, buttons, tab, and null byte
char input_display[13];  // \t | A B S T U D L R | \t \0

#define PRE_FRAME_OFFSET 2 // number of frames to ignore when powered on

// mario 3 has about 120 and 157 microseconds between each latch
// blaster master has about 157 us
// 400us divided by 4us (prescaler of 64)
const int LATCH_TIMEOUT = 400 / 4;

// length of one frame divided by 4 (prescaler of 64)
// 1000000us / 60fps / 4us
const int LAG_TIMEOUT_60HZ = 1000000 / 60 / 4;

// 1000000us / 60.09fps / 4us (alternate timing)
const int LAG_TIMEOUT_60HZ09 = 1000000 / 60.09 / 4;

char *game_title_list[] =
{
  "Super Mario Bros. / Duck Hunt",
  "Super Mario Bros. 3",
  "Blaster Master",
  "Rad Racer",
  "Metroid",
  "Dr. Mario",
  "Unknown"
};

char *game_name_list[] =
{
  "smbdh",
  "smb3",
  "blastermaster",
  "radracer",
  "metroid",
  "drmario",
  "unknown"
};

void setup()
{
  // All of our simulated buttons need to be outputs
  pinMode(RIGHT, OUTPUT);
  pinMode(LEFT, OUTPUT);
  pinMode(DOWN, OUTPUT);
  pinMode(UP, OUTPUT);
  pinMode(START, OUTPUT);
  pinMode(SELECT, OUTPUT);
  pinMode(B, OUTPUT);
  pinMode(A, OUTPUT);

  pinMode(SWITCH, INPUT_PULLUP);

  // Start with buttons completely off
  writeButtons(0);

  // Enable status LED pin and turn it off
  pinMode(GREEN_LED, OUTPUT);
  digitalWrite(GREEN_LED, LOW);

  // Enable lag LED pin and turn it off
  pinMode(WHITE_LED, OUTPUT);
  digitalWrite(WHITE_LED, LOW);

  game_detect = digitalRead(SWITCH);
  game_detect ^=  1;  // invert it so GND is HIGH

  Serial.begin(19200);

  // initialize the SD card at SPI_HALF_SPEED to avoid bus errors with
  // breadboards. use SPI_FULL_SPEED for better performance.
  if (!sd.begin(chipSelect, SPI_HALF_SPEED)) {
    digitalWrite(WHITE_LED, HIGH);
    sd.initErrorPrint();
  }

  current_game = EEPROM.read(0);
  current_movie = EEPROM.read(1);

  getGameFileCount(current_game);  // get how many movie files there are

  if (game_detect)
  {
    if (current_movie < (total_game_movies - 1))
      current_movie++;
    else
      current_movie = 0;

    EEPROM.write(1, current_movie);

    if (current_movie < 8)
    {
      writeButtons(1 << current_movie);
      delay(200);
      writeButtons(0);
      delay(200);
      writeButtons(1 << current_movie);
      delay(200);
      writeButtons(0);
      delay(200);
      writeButtons(1 << current_movie);
      delay(200);
    }
  }

  getGameFileName(current_movie);  // get filename of index number

  if (!movie_file.open(filename, O_READ)) {
    sd.errorPrint("failed to open movie file");
    digitalWrite(WHITE_LED, HIGH);
    Serial.println(F("done."));
    while (1) {};
  }

  latch_timeout = LATCH_TIMEOUT;
  frame = PRE_FRAME_OFFSET - 1;

  movie_length = movie_file.fileSize();

  // read data from SD card into buffer
  loadSDbuffer();

  // clear Timer/Counter1 settings, used for updating frames
  TCCR1A = 0;
  TCCR1B = (1 << WGM12);  // mode 4: CTC
  TIFR1 = (1 << OCF1A);  // clear match that occured with 0

  //Timer/Counter1 Output Compare A Match Interrupt Enable
  TIMSK1 |= (1 << OCIE1A);

  // We are now ready to talk to the console, so turn on interrupts
  attachInterrupt(digitalPinToInterrupt(LATCH_IN), latch_pulse, FALLING);

  if (game_detect)
  {
    Serial.println(F("READY to detect game"));

    // Turn on the lag LED to announce our readiness
    digitalWrite(WHITE_LED, HIGH);
  }
  else
  {
    Serial.println(filename);
    Serial.print(F("READY to play "));
    Serial.println(game_title_list[current_game]);

    // Turn on the status LED to announce our readiness
    digitalWrite(GREEN_LED, HIGH);
  }
}

void getGameFileName(unsigned char count)
{
  unsigned char game_movie_index = 0;

  // reset openNext
  sd.chdir();

  while (movie_file.openNext(sd.vwd(), O_READ))
  {
    movie_file.getName(filename, sizeof(filename));  // get filename in char array
    String str = filename;  // get filename for string functions

    if (movie_file.isDir())  // skip directories
    {
      movie_file.close();
      continue;
    }

    movie_file.close();

    // count filenames that begin with game_name_list ("smbdh", "smb3", etc)
    if (str.startsWith(game_name_list[current_game]))
    {
      // return if we've selected filename number count
      if (count == game_movie_index || count >= total_game_movies)
        return;

      game_movie_index++;
    }
  }
}

void getGameFileCount(unsigned char count)
{
  total_game_movies = 0;

  // reset openNext
  sd.chdir();

  while (movie_file.openNext(sd.vwd(), O_READ))
  {
    movie_file.getName(filename, sizeof(filename));  // get filename in char array
    String str = filename;  // get filename for string functions

    if (movie_file.isDir())  // skip directories
    {
      movie_file.close();
      continue;
    }

    movie_file.close();

    // we found a filename starting with game_name_list ("smbdh", "smb3", etc)
    if (str.startsWith(game_name_list[count]))
      total_game_movies++;
  }
}


void latch_pulse()
{
  // reset the timer value each latch
  TCNT1 = 0;

  if (first_latch)
  {
    first_latch = 0;
    frame_length = 0;
  }

  frame_total += frame_length;

  // set latch timeout
  OCR1A = latch_timeout;  // set to trigger after last latch

  // start timer
  TCCR1B |= (0 << CS12) | (1 << CS11) | (1 << CS10);  // 64 prescaler

  // reset counter so movie doesn't stop
  lag_frames = 0;


  if (!game_detect)
  {
    // Flash the status led every 5 latches (~6 times a second)
    if (frame % 10 == 0)
      digitalWrite(GREEN_LED, HIGH);

    if (frame % 10 == 5)
      digitalWrite(GREEN_LED, LOW);
  }
}


ISR(TIMER1_COMPA_vect)
{
  // a timer1 match occurred, timer is reset to 0 automatically
  // latches are over or we counted out a lag frame
  frame_total += frame_length;
  frame_length = 0;


  // the movie is over, disable the bot
  if (file_loc >= movie_length || lag_frames > 90)
  {
    TCCR1B = 0;  //stop Timer/Counter1
    detachInterrupt(digitalPinToInterrupt(LATCH_IN));  // stop reading latch pulse from NES
    TIMSK1 &= ~(1 << OCIE1A);  // disable Timer/Counter1 Output Compare A Match Interrupt

    // set all buttons/LEDs to off
    writeButtons(0);

    if (lag_frames > 90)
    {
      prev_buttons = 0;
      printInfo();
      Serial.println(F("NES turned off."));
    }
    else
    {
      prev_buttons = next_buttons;
      printInfo();
      Serial.println(F("Done."));
    }

    digitalWrite(GREEN_LED, LOW);
    digitalWrite(WHITE_LED, HIGH);

    return;
  }


  // it's a lag frame, it counted out long enough to be a lag_timeout
  if (OCR1A > latch_timeout)
  {
    // this is a lag frame
    lag_frame = 1;

    //lag_timeout = LAG_TIMEOUT_60HZ09;
    lag_timeout = LAG_TIMEOUT_60HZ;

    OCR1A = lag_timeout;

    if (game_detect)
    {
      if (frame < 40)
        detectGame();

      frame++;
      return;
    }

    /* // custom patch for mario 3 level 6-8
      if (file_loc - 1 == 104500)
      {
      if (current_movie == smb3)
      {
        getSDbyte();
        frame--;
        Serial.println("fixed");
      }
      } */

    // set this flag for the next latch pulse so it's the first
    first_latch = 1;

    // if too many lag frames, console must be off
    lag_frames++;

    // important work is done, now we can send data over the serial port
    prev_buttons = 0;
    printInfo();

    frame++;
    digitalWrite(GREEN_LED, LOW);
    digitalWrite(WHITE_LED, HIGH);
  }


  //single or multiple latches are over
  if (OCR1A == latch_timeout)
  {
    // this is not a lag frame
    lag_frame = 0;

    //frame_length = 0;
    lag_timeout = LAG_TIMEOUT_60HZ;

    // set counter to lag timeout in case a latch doesn't happen next time
    OCR1A = lag_timeout;

    if (game_detect)
    {
      if (frame < 40)
        detectGame();

      unsigned char game;

      if (frame == 40)
      {
        writeButtons(0);

        Serial.print(F("Detected Game: "));
        if (gameDetectLagCount == 14 || gameDetectLagCount == 15)
        {
          game = 0;  // smbdh
          Serial.println(game_title_list[0]);
        }

        else if (gameDetectLagCount == 10 || gameDetectLagCount == 9)
        {
          game = 1;  // smb3
          Serial.println(game_title_list[1]);
        }

        else if (gameDetectLagCount == 5 || gameDetectLagCount == 6)
        {
          game = 2;  // blastermaster
          Serial.println(game_title_list[2]);
        }

        else if (gameDetectLagCount == 27)
        {
          game = 3;  // radracer
          Serial.println(game_title_list[3]);
        }

        else if (gameDetectLagCount == 17 || gameDetectLagCount == 18)
        {
          game = 4;  // metroid
          Serial.println(game_title_list[4]);
        }

        else if (gameDetectLagCount == 3 || gameDetectLagCount == 4)
        {
          game = 5;  // drmario
          Serial.println(game_title_list[5]);
        }

        else
        {
          game = 0;
          Serial.print(F("Unknown. gameDetectLagCount: "));
          Serial.println(gameDetectLagCount);
        }
        Serial.print(F("gameDetectLagCount = "));
        Serial.println(gameDetectLagCount);
        EEPROM.write(0, game);

        writeButtons(1 << game);
      }
      frame++;
      return;
    }

    // set this flag for the next latch pulse so we know it's the first
    first_latch = 1;

    // copy previous buttons for printInfo()
    prev_buttons = next_buttons;

    // get next buttons and write out to 4021
    next_buttons = getSDbyte();
    writeButtons(next_buttons);

    // important work is done, now we can send data over the serial port
    if (frame > PRE_FRAME_OFFSET)
      printInfo();

    frame++;
    file_loc++;

    digitalWrite(WHITE_LED, LOW);
  }

  frame_total = 0;
}

void detectGame()
{
  if (lag_frame)
    gameDetectLagCount++;

  if (frame % 2)  // press Start or A every other frame to advance past title screen
  {
    next_buttons = 0x08;  // start button
    writeButtons(next_buttons);
  }
  else
  {
    next_buttons = 0x01;  // a button
    writeButtons(next_buttons);
  }
}


void loop()
{
  if (!SD_buffer_full)
  {
    while (SD_buffer_pos > 0)
    {
      if (SD_buffer_pos > 1)
      {
        Serial.println(F("Buffer position reached 2"));
        //file_loc = movie_length;
      }
      SD_buffer[0] = SD_buffer[1];
      SD_buffer[1] = SD_buffer[2];
      SD_buffer[2] = SD_buffer[3];
      SD_buffer[3] = SD_buffer[4];
      SD_buffer[4] = movie_file.read();
      SD_buffer_pos--;
    }
  }

  frame_length = TCNT1;
}


void printInfo()
{
  String outputString;
  String buttonString = displayButtons(prev_buttons);
  int pulse_spacing = (frame_total << 2) - (latch_timeout << 2);

  if (frame < PRE_FRAME_OFFSET)
    outputString = String(frame + buttonString + "\t offset");
  else if (lag_frame)
    outputString = String(frame + buttonString + "\t" + (frame_total << 2) + "\t lag");
  else
  {
    if (file_loc > 0)
      //if (pulse_spacing > 0)
      outputString = String(frame + buttonString + (file_loc - 1) + "\t" + pulse_spacing);
    //else
    //  outputString = String(frame + buttonString + (file_loc - 1) + "\t0");
    else
      outputString = String(frame + buttonString + file_loc);
  }

  Serial.println(outputString);
}


byte getSDbyte()
{
  unsigned char SD_byte = SD_buffer[SD_buffer_pos];
  SD_buffer_pos++;

  SD_buffer_full = 0;

  return SD_byte;
}


void loadSDbuffer()
{
  SD_buffer[0] = movie_file.read();
  SD_buffer[1] = movie_file.read();
  SD_buffer[2] = movie_file.read();
  SD_buffer[3] = movie_file.read();
  SD_buffer[4] = movie_file.read();

  SD_buffer_full = 1;
  SD_buffer_pos = 0;
}


char * displayButtons(byte buttons)
{
  char input_array[] = "ABSTUDLR";

  input_display[0] = '\t';
  input_display[1] = '|';

  for (int count = 2; count < 10; count++)
  {
    // first button is A which is LSB
    if (buttons & 1)
      input_display[count] = input_array[count - 2];
    else
      input_display[count] = ' ';

    buttons = buttons >> 1;
  }

  input_display[10] = '|';
  input_display[11] = '\t';
  input_display[12] = '\0';  // a null byte signals end of text string

  return input_display;
}


// Write the given button sequence to the shift register
void writeButtons(byte buttons)
{
  // Shift out the button information from the data byte
  // and make each pin HIGH or LOW accordingly
  buttons = ~buttons;

  digitalWrite(A,      buttons & 1);
  buttons = buttons >> 1;
  digitalWrite(B,      buttons & 1);
  buttons = buttons >> 1;
  digitalWrite(SELECT, buttons & 1);
  buttons = buttons >> 1;
  digitalWrite(START,  buttons & 1);
  buttons = buttons >> 1;
  digitalWrite(UP,     buttons & 1);
  buttons = buttons >> 1;
  digitalWrite(DOWN,   buttons & 1);
  buttons = buttons >> 1;
  digitalWrite(LEFT,   buttons & 1);
  buttons = buttons >> 1;
  digitalWrite(RIGHT,  buttons & 1);
}

