#!/usr/bin/env ruby
#
# Test program to control the "Brother P-touch Home & Hobby"
# using the RemoteKeyboard AVR program
# Ned Konz, ned@bike-nomad.com
#
require 'serialport'

module RemoteKeyboard

  # Serial interface to RemoteKeyboard
  class SerialInterface
    def initialize(_port, _baud)
      @port = SerialPort.new(_port, _baud, 8, 1, SerialPort::NONE)
      @last_read = nil
      @last_write = nil
    end

    attr_reader :last_read, :last_write

    def close
      @port.close unless @port.closed?
    end

    def read_data_available?(timeout = 0)
      (rh, wh, eh) = IO::select([@port], nil, nil, timeout)
      ! rh.nil?
    end

    def read_all_available_bytes(timeout =0, max = 1000)
      if read_data_available?(timeout)
        r = @port.sysread(max)
        @last_read = Time.now
        r
      else
        ''
      end
    end

    def write(data)
      @port.write(data)
      @last_write = Time.now
    end
  end

  # Representation of single key
  class Key
    def initialize(_code, _str, _kbd)
      @code = '%02d' % _code.to_i 
      @str = _str
      @pressed = false
      @kbd = _kbd
    end

    attr_reader :str, :code

    def pressed=(state)
      @pressed = state
    end

    def pressed
      @pressed
    end

    # returns string or nil if already pressed
    def press
      unless pressed
        pressed= true
        return "p" + @code + "\r"
      end
    end

    # returns string or nil if already released
    def release
      if pressed
        pressed= false
        return "r" + @code + "\r"
      end
    end

    def modify(s)
      s
    end
  end

  class ShiftKey < Key
    def press
      unless pressed
        super
        @kbd.shift(true)
      end
    end

    def release
      if pressed
        super
        @kbd.shift(false)
      end
    end
  end

  class TogglingShiftKey < ShiftKey
  end

  class Keyboard
    @@keys = {}
    @@shifted_keys = {}

    def initialize(_port, _baud)
      @serial = SerialInterface.new(_port, _baud)
      @pressed = []
      @forced = []
      shift(false)
    end

    def shift(b)
      if b
        @currentKeys = @@shifted_keys
      else
        @currentKeys = @@keys
      end
    end

    attr_reader :serial

    # interpret the output from the keyboard watcher
    def interpretKeyboard(str)
      interpreted = ''
      str.gsub!(/\s*([pr])(\d\d)\s*/) do |m|
        key = @currentKeys[$2]
        if key
          if $1 == 'p'  # press
            key.press
            interpreted += key.str
            @pressed << key
          else # $1 == 'r' # release
            key.release
            @pressed.delete(key)
          end
        else
          $stderr.puts("unknown key #{m.inspect}")
        end
        ''
      end
      return interpreted, str
    end

    # translate the given string into commands for press/release
    def driveKeyboard(str)
    end

  end

  class BrotherPTouchHomeAndHobby < Keyboard
    # row/column	key   code-shift
    # Printing characters
    KEY_DEFS = [
    [ 72, ' ', 'Feed' ],
    [ 71, ',', ':' ],
    [ 42, '.', '?' ],
    [ 53, "'", '/' ],
    [ 02, '"', '!' ],
    [ 54, '&', 'Size' ],
    [ 64, 'A', 'Style' ],
    [ 13, 'B', 'cent' ],
    [ 73, 'C', '@' ],
    [ 74, 'D', 'Frame' ],
    [ 35, 'E', '3' ],
    [ 24, 'F', 'Accent' ],
    [ 14, 'G', 'Repeat' ],
    [ 44, 'H', 'Check' ],
    [ 05, 'I', '8' ],
    [ 04, 'J', 'Preset' ],
    [ 01, 'K', '(' ],
    [ 41, 'L', ')' ],
    [ 03, 'M', '.' ],
    [ 43, 'N', '<telephone>' ],
    [ 11, 'O', '9' ],
    [ 21, 'P', '0' ],
    [ 55, 'Q', '1' ],
    [ 75, 'R', '4' ],
    [ 34, 'S', 'Underline' ],
    [ 25, 'T', '5' ],
    [ 45, 'U', '7' ],
    [ 23, 'V', '$' ],
    [ 65, 'W', '2' ],
    [ 33, 'X', '~' ],
    [ 15, 'Y', '6' ],
    [ 63, 'Z', '-' ],
    [ 22, 'Return' ]]
    CONTROL_KEYS = [
    # Control keys
    [ 31, 'BS', 'Clear' ],
    [ 51, 'LArrow', 'Home' ],
    [ 61, 'RArrow', 'End' ],
    [ 60, 'Sym' ]
    ]
    # Modifier keys
    MODIFIER_KEYS = [
    [ 52, 'LCode' ],
    [ 12, 'RCode' ]
    ]
    # sticky mod keys
    STICKY_MOD_KEYS = [
    [ 62, 'Caps' ],
    [ 32, 'Num' ]
    ]
    SPECIAL_KEYS = [
    [ 06, 'Power' ],
    [ 50, 'Print' ]
    ]

    self.init

    def initialize(_port, _baud)
      super
      KEY_DEFS.each do |a|
        self.class.addKey(Key.new(a[0], a[1]))
        self.class.addShiftedKey(Key.new(a[0], a[2]))
      end
    end
  end

end

if __FILE__ == $0

include RemoteKeyboard

port = Dir["/dev/cu.usb*"][0]
baud = 38400

$kbd = BrotherPTouchHomeAndHobby.new(port, baud)

bytes = ''
while true do
  bytes += $kbd.serial.read_all_available_bytes
  str, bytes = $kbd.interpretKeyboard(bytes)
  p str if str.length > 0
end

end
