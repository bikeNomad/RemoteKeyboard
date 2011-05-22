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
      @port = SerialPort.new(_port)
      @port.set_modem_params(_baud, 8, 1, SerialPort::NONE)
      @port.flow_control = SerialPort::NONE 
      @baud = _baud
      @last_read = @next_write = Time.now
      @write_delay = 0.1
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
      now = Time.now
      if now < @next_write
        sleep(@next_write - now)
      end
      @port.write(data)
      # $stderr.puts("write #{data.inspect}")   # DEBUG
      @next_write = Time.now + (data.size / @baud) + @write_delay
    end
  end

  # keyboard with shift key, caps and num lock, and editing keys
  class Keyboard

    def initialize(_port, _baud)
      @serial = SerialInterface.new(_port, _baud)
      # code => string or symbol
      # string or symbol => code
      @unshiftedKeys = {}
      # code => string or symbol
      # string or symbol => code
      @shiftedKeys = {}
      @powerState = false
      @shiftState = false
      @capsLockState = false
      @numLockState = false
    end

    # interpret the output from the keyboard watcher
    def interpretKeyboard(str)
      interpreted = ''
      str.gsub!(/\s*([pr])(\d\d)\s*/) do |m|
        if $1 == 'p'  # press
          interpreted += pressed($2).to_s
        else # $1 == 'r' # release
          released($2)
        end
        ''
      end
      return interpreted, str
    end

    # translate the given string into commands for press/release
    def driveKeyboard(str)
      str.chomp.split('').each do |s|
        lastShiftState = @shiftState
        lastCapsLockState = @capsLockState
        lastNumLockState = @numLockState
        code = nil
        case s
          when /[0-9]/
            code = @shiftedKeys[s]
            if !lastNumLockState || !lastShiftState
              press(@unshiftedKeys[:shift])
            end
              press(code)
              release(code)
            if !lastNumLockState || !lastShiftState
              release(@unshiftedKeys[:shift])
            end

          when /[A-Za-z]/
            code = @unshiftedKeys[s.upcase]
            if lastNumLockState
              press(@unshiftedKeys[:num_lock])
              release(@unshiftedKeys[:num_lock])
              num_lock(true)
            end
            if !lastCapsLockState && /[A-Z]/.match(s) ||
              (lastCapsLockState && /[a-z]/.match(s))
                press(@unshiftedKeys[:caps_lock])
                release(@unshiftedKeys[:caps_lock])
                caps_lock(true)
            end
            press(code)
            release(code)

          when /[\x15]/ # ctrl-U
            if !lastShiftState
              press(@unshiftedKeys[:shift])
            end
            press(@shiftedKeys[:clear])
            release(@shiftedKeys[:clear])
            if !lastShiftState
              release(@unshiftedKeys[:shift])
            end

          else
            code = @unshiftedKeys[s] || @shiftedKeys[s]
            press(code)
            release(code)
        end
      end
    end

    def read_all_available_stdin_bytes(timeout = 0)
      (rh, wh, eh) = IO::select([$stdin], nil, nil, timeout)
      if rh
        $stdin.sysread(1000)
      end
    end

    def monitorKeyboard
      serial.read_all_available_bytes
      bytes = ''
      last_ibytes = last_obytes = ''
      last_str = ''
      while true do
        bytes += serial.read_all_available_bytes
        str = ''
        bytes.sub!("RemoteKeyboard v1.0 by Ned Konz\r\n", '')
        bytes.gsub!("\x00", '')
        if bytes.length > 0
#          puts "IBYTES=#{bytes.inspect}" unless bytes == last_ibytes
          last_ibytes = bytes
          str, bytes = interpretKeyboard(bytes)
#          puts "STR=#{str.inspect}" unless str == last_str
#          puts "OBYTES=#{bytes.inspect}" unless bytes == last_obytes
          print(str)
        end
        last_str = str
        last_obytes = bytes

        sbytes = read_all_available_stdin_bytes
        if sbytes && sbytes.length > 0
          driveKeyboard(sbytes)
        end
      end
    end

  protected
    attr_reader :serial

    def add_key(keycode, unshifted, shifted)
      @unshiftedKeys[keycode] = unshifted
      @unshiftedKeys[unshifted] = keycode
      @shiftedKeys[keycode] = shifted
      @shiftedKeys[shifted] = keycode
    end

    def meaningOfCode(keycode, *args)
      key = (@shiftState || \
            (@numLockState && /[0-9]/.match(@shiftedKeys[keycode]))) \
        ? @shiftedKeys[keycode] : @unshiftedKeys[keycode]
      case key
        when nil
        when Symbol
          if self.respond_to?(key)
            v = self.send(key, *args)
            key = v || "<#{key}>"
          else
            key = "<U #{key}>"
          end
        when String
#         $stderr.puts "#{keycode} #{key} CL=#{@capsLockState} NL=#{@numLockState}"
          if /[A-Za-z]/.match(key)
            key = @capsLockState ? key.upcase : key.downcase
          end
        else
          $stderr.puts("unknown key def #{keycode.inspect} => #{key.inspect}")
      end
      key
    end

    # returns string representation of key
    def pressed(keycode)
      key = meaningOfCode(keycode, true)
      if key.nil?
        $stderr.puts("pressed unknown key #{keycode.inspect}")
      end
      key
    end

    def released(keycode)
      key = meaningOfCode(keycode, false)
      if key.nil?
        $stderr.puts("released unknown key #{keycode.inspect}")
      end
      key
    end

    def press(keycode)
      serial.write(sprintf("p%s\r", keycode))
    end

    def release(keycode)
      serial.write(sprintf("r%s\r", keycode))
    end

    # special keys

    def shift(press)
      @shiftState = press
      ""
    end

    def caps_lock(press)
      @capsLockState = !@capsLockState if press
      ""
    end

    def num_lock(press)
      @numLockState = !@numLockState if press
      ""
    end

    def power(press)
      @powerState = !@powerState if press
      nil
    end

    def backspace(press)
      press ? "\x08" : ""
    end

    def clear(press)
      press ? "\r" : ""
    end

  end # class Keyboard

  class BrotherPTouchHomeAndHobby < Keyboard
    # row/column	key   code-shift
    # Printing characters
    KEY_DEFS = [
      [ '72', ' ', :feed ],
      [ '71', ',', ':' ],
      [ '42', '.', '?' ],
      [ '53', "'", '/' ],
      [ '02', '"', '!' ],
      [ '54', '&', :size ],
      [ '64', 'A', :style ],
      [ '13', 'B', '<cent>' ],
      [ '73', 'C', '@' ],
      [ '74', 'D', :frame ],
      [ '35', 'E', '3' ],
      [ '24', 'F', :accent ],
      [ '14', 'G', :repeat ],
      [ '44', 'H', :check ],
      [ '05', 'I', '8' ],
      [ '04', 'J', :preset ],
      [ '01', 'K', '(' ],
      [ '41', 'L', ')' ],
      [ '03', 'M', '.' ],
      [ '43', 'N', '<telephone>' ],
      [ '11', 'O', '9' ],
      [ '21', 'P', '0' ],
      [ '55', 'Q', '1' ],
      [ '75', 'R', '4' ],
      [ '34', 'S', :underline ],
      [ '25', 'T', '5' ],
      [ '45', 'U', '7' ],
      [ '23', 'V', '$' ],
      [ '65', 'W', '2' ],
      [ '33', 'X', '~' ],
      [ '15', 'Y', '6' ],
      [ '63', 'Z', '-' ],
      [ '22', "\n", "\n" ],
      # Control keys
      [ '31', :backspace, :clear ],
      [ '51', :l_arrow, :home ],
      [ '61', :r_arrow, :end ],
      [ '60', :symbol, :symbol ],
      # Shift keys
      [ '52', :shift, :shift ],
      [ '12', :shift, :shift ],
      # sticky mod keys
      [ '62', :caps_lock, :caps_lock ],
      [ '32', :num_lock, :num_lock ],
      # special keys
      [ '06', :power, :power ],
      [ '50', :print, :print ]
    ]

    def initialize(_port, _baud)
      super
      KEY_DEFS.map { |a| add_key(*a) }
    end

  end # class BrotherPTouchHomeAndHobby

end # module RemoteKeyboard

if __FILE__ == $0

include RemoteKeyboard

port = Dir["/dev/cu.usb*"][0]
if port.nil?
  $stderr.puts "Can't find serial port!"
  exit 1
else
  $stderr.puts "Using serial port #{port}"
end

baud = 38400

$kbd = BrotherPTouchHomeAndHobby.new(port, baud)
$kbd.monitorKeyboard

end
