#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

constexpr std::string_view DEFAULT_WORD = "abcdefghijklmnopqrstuvwxyz";
constexpr int FRAME_TIMEOUT_MS = 20;
constexpr int FLASH_DURATION_MS = 300;

constexpr std::string_view ANSI_RESET = "\033[0m";
constexpr std::string_view ANSI_CLEAR_SCREEN = "\033[2J\033[H";
constexpr std::string_view ANSI_HIDE_CURSOR = "\033[?25l";
constexpr std::string_view ANSI_SHOW_CURSOR = "\033[?25h";
constexpr std::string_view ANSI_ALT_SCREEN_ON = "\033[?1049h";
constexpr std::string_view ANSI_ALT_SCREEN_OFF = "\033[?1049l";
constexpr std::string_view ANSI_BOLD = "\033[1m";
constexpr std::string_view ANSI_DIM = "\033[2m";
constexpr std::string_view ANSI_UNDERLINE = "\033[4m";
constexpr std::string_view ANSI_RED = "\033[31m";
constexpr std::string_view ANSI_GREEN = "\033[32m";
constexpr std::string_view ANSI_YELLOW = "\033[33m";
constexpr std::string_view ANSI_CYAN = "\033[36m";
constexpr std::string_view ANSI_WHITE = "\033[37m";
constexpr std::string_view ANSI_STRIKETHROUGH = "\033[9m";
constexpr std::string_view ANSI_BG_RED = "\033[41;37m";
constexpr std::string_view ANSI_BG_GREEN = "\033[42;30m";
constexpr std::string_view ANSI_INVERSE = "\033[7m";

enum class MistakeAction {
    Nothing,
    Prevent,
    Erase,
};

enum class FocusMode {
    Practice,
    EditWord,
};

enum class FlashKind {
    None,
    Error,
    Success,
    NewRecord,
};

enum class SoundEffect {
    Keystroke,
    Mistake,
    Success,
    NewRecord,
};

enum class AudioBackendType {
    None,
    Paplay,
    Aplay,
    Ffplay,
};

enum class InputEventType {
    None,
    Character,
    Backspace,
    ToggleFocus,
    CycleMistakeAction,
    ToggleSound,
    ToggleMistakeFeedback,
    ToggleHideText,
    Quit,
};

struct InputEvent {
    InputEventType type = InputEventType::None;
    std::string utf8_text;
};

struct Statistics {
    std::uint64_t correct_count = 0;
    std::uint64_t mistake_count = 0;
    std::uint64_t words_completed = 0;
    std::uint64_t word_attempts = 0;
    std::vector<double> completion_times_seconds;
};

struct AudioBackend {
    AudioBackendType backend_type = AudioBackendType::None;
    std::string executable_path;
};

struct AudioAssetPaths {
    std::string keystroke_wav_path;
    std::string mistake_wav_path;
    std::string success_wav_path;
    std::string new_record_wav_path;
};

/** Returns true when the byte is a UTF-8 continuation byte. */
bool isUtf8ContinuationByte(const unsigned char input_byte) {
    return (input_byte & 0b1100'0000U) == 0b1000'0000U;
}

/** Returns the expected UTF-8 sequence length for a leading byte. */
std::size_t getExpectedUtf8SequenceLength(const unsigned char leading_byte) {
    if ((leading_byte & 0b1000'0000U) == 0U) {
        return 1;
    }
    if ((leading_byte & 0b1110'0000U) == 0b1100'0000U) {
        return 2;
    }
    if ((leading_byte & 0b1111'0000U) == 0b1110'0000U) {
        return 3;
    }
    if ((leading_byte & 0b1111'1000U) == 0b1111'0000U) {
        return 4;
    }
    assert(false && "Expected a valid UTF-8 leading byte, got an invalid leading byte");
    std::abort();
}

/** Splits a UTF-8 string into code point substrings. Return shape: (code_point_count). */
std::vector<std::string> splitUtf8CodePoints(const std::string& utf8_text) {
    std::vector<std::string> code_points;
    std::size_t byte_index = 0;

    while (byte_index < utf8_text.size()) {
        const unsigned char leading_byte = static_cast<unsigned char>(utf8_text[byte_index]);
        const std::size_t sequence_length = getExpectedUtf8SequenceLength(leading_byte);
        assert(
            byte_index + sequence_length <= utf8_text.size() &&
            "Expected a complete UTF-8 sequence, got a truncated sequence"
        );

        // Loop invariant: every earlier byte was consumed into exactly one valid UTF-8 code point.
        for (std::size_t continuation_index = 1; continuation_index < sequence_length; ++continuation_index) {
            const unsigned char continuation_byte =
                static_cast<unsigned char>(utf8_text[byte_index + continuation_index]);
            assert(
                isUtf8ContinuationByte(continuation_byte) &&
                "Expected a UTF-8 continuation byte, got a non-continuation byte"
            );
        }

        code_points.emplace_back(utf8_text.substr(byte_index, sequence_length));
        byte_index += sequence_length;
    }

    return code_points;
}

/** Counts UTF-8 code points. */
std::size_t countUtf8CodePoints(const std::string& utf8_text) {
    return splitUtf8CodePoints(utf8_text).size();
}

/** Removes the last UTF-8 code point in-place. side-effects: mutates `utf8_text`. */
void popLastUtf8CodePoint(std::string& utf8_text) {
    if (utf8_text.empty()) {
        return;
    }

    std::size_t new_size = utf8_text.size() - 1;
    while (new_size > 0 && isUtf8ContinuationByte(static_cast<unsigned char>(utf8_text[new_size]))) {
        --new_size;
    }

    utf8_text.resize(new_size);
}

/** Returns true when `prefix_candidate` is a byte prefix of `full_text`. */
bool startsWithBytes(const std::string& full_text, const std::string& prefix_candidate) {
    return full_text.rfind(prefix_candidate, 0) == 0;
}

/** Returns a masked string with one asterisk per UTF-8 code point. */
std::string maskUtf8Text(const std::string& utf8_text) {
    return std::string(countUtf8CodePoints(utf8_text), '*');
}

/** Returns true when the given executable path exists and is executable. */
bool isExecutableAvailable(const std::string& executable_path) {
    return access(executable_path.c_str(), X_OK) == 0;
}

/** Returns the first supported local audio backend. */
AudioBackend detectAudioBackend() {
    const std::array<AudioBackend, 3> audio_backend_candidates {{
        AudioBackend {AudioBackendType::Paplay, "/usr/bin/paplay"},
        AudioBackend {AudioBackendType::Aplay, "/usr/bin/aplay"},
        AudioBackend {AudioBackendType::Ffplay, "/usr/bin/ffplay"},
    }};

    for (const AudioBackend& audio_backend_candidate : audio_backend_candidates) {
        if (isExecutableAvailable(audio_backend_candidate.executable_path)) {
            return audio_backend_candidate;
        }
    }

    return AudioBackend {};
}

/** Returns a shell-safe single-quoted string. */
std::string quoteForShell(const std::string& raw_text) {
    std::string quoted_text = "'";
    for (const char raw_character : raw_text) {
        if (raw_character == '\'') {
            quoted_text += "'\\''";
        } else {
            quoted_text.push_back(raw_character);
        }
    }
    quoted_text += "'";
    return quoted_text;
}

/** Appends a 16-bit little-endian integer to a byte buffer. */
void appendLittleEndian16(std::vector<std::uint8_t>& output_bytes, const std::uint16_t value) {
    output_bytes.push_back(static_cast<std::uint8_t>(value & 0x00FFU));
    output_bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0x00FFU));
}

/** Appends a 32-bit little-endian integer to a byte buffer. */
void appendLittleEndian32(std::vector<std::uint8_t>& output_bytes, const std::uint32_t value) {
    output_bytes.push_back(static_cast<std::uint8_t>(value & 0x000000FFU));
    output_bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0x000000FFU));
    output_bytes.push_back(static_cast<std::uint8_t>((value >> 16U) & 0x000000FFU));
    output_bytes.push_back(static_cast<std::uint8_t>((value >> 24U) & 0x000000FFU));
}

/** Builds a mono WAV file. Return shape: (byte_count). */
std::vector<std::uint8_t> createSineWaveWavBytes(
    const int sample_rate_hz,
    const double frequency_hz,
    const double duration_seconds,
    const double amplitude_ratio
) {
    assert(sample_rate_hz > 0 && "Expected a positive sample rate, got a non-positive sample rate");
    assert(frequency_hz > 0.0 && "Expected a positive frequency, got a non-positive frequency");
    assert(duration_seconds > 0.0 && "Expected a positive duration, got a non-positive duration");
    assert(
        amplitude_ratio >= 0.0 && amplitude_ratio <= 1.0 &&
        "Expected amplitude in [0, 1], got a value outside [0, 1]"
    );

    const std::uint16_t channel_count = 1;
    const std::uint16_t bits_per_sample = 16;
    const std::uint16_t bytes_per_sample = bits_per_sample / 8U;
    const std::uint32_t sample_count = static_cast<std::uint32_t>(
        std::llround(static_cast<double>(sample_rate_hz) * duration_seconds)
    );
    const std::uint32_t byte_rate =
        static_cast<std::uint32_t>(sample_rate_hz) * channel_count * bytes_per_sample;
    const std::uint16_t block_align = channel_count * bytes_per_sample;
    const std::uint32_t data_chunk_size = sample_count * block_align;
    const std::uint32_t riff_chunk_size = 36U + data_chunk_size;
    const double pi_value = std::acos(-1.0);
    const double max_amplitude = amplitude_ratio * 32767.0;

    std::vector<std::uint8_t> wav_bytes;
    wav_bytes.reserve(static_cast<std::size_t>(44U + data_chunk_size));

    wav_bytes.insert(wav_bytes.end(), {'R', 'I', 'F', 'F'});
    appendLittleEndian32(wav_bytes, riff_chunk_size);
    wav_bytes.insert(wav_bytes.end(), {'W', 'A', 'V', 'E'});
    wav_bytes.insert(wav_bytes.end(), {'f', 'm', 't', ' '});
    appendLittleEndian32(wav_bytes, 16U);
    appendLittleEndian16(wav_bytes, 1U);
    appendLittleEndian16(wav_bytes, channel_count);
    appendLittleEndian32(wav_bytes, static_cast<std::uint32_t>(sample_rate_hz));
    appendLittleEndian32(wav_bytes, byte_rate);
    appendLittleEndian16(wav_bytes, block_align);
    appendLittleEndian16(wav_bytes, bits_per_sample);
    wav_bytes.insert(wav_bytes.end(), {'d', 'a', 't', 'a'});
    appendLittleEndian32(wav_bytes, data_chunk_size);

    // Loop invariant: the WAV buffer already contains the header plus all earlier PCM samples.
    for (std::uint32_t sample_index = 0; sample_index < sample_count; ++sample_index) {
        const double sample_time_seconds = static_cast<double>(sample_index) / static_cast<double>(sample_rate_hz);
        const double phase_radians = 2.0 * pi_value * frequency_hz * sample_time_seconds;
        const double envelope_ratio = 1.0 - (static_cast<double>(sample_index) / static_cast<double>(sample_count));
        const double sample_value = std::sin(phase_radians) * max_amplitude * envelope_ratio;
        const auto pcm_sample = static_cast<std::int16_t>(std::llround(sample_value));
        appendLittleEndian16(wav_bytes, static_cast<std::uint16_t>(pcm_sample));
    }

    return wav_bytes;
}

/** Writes binary bytes to a file path. side-effects: creates or overwrites the given file. */
void writeBinaryFile(const std::string& output_path, const std::vector<std::uint8_t>& file_bytes) {
    std::ofstream output_stream(output_path, std::ios::binary | std::ios::trunc);
    assert(output_stream.good() && "Expected output file to open successfully, got a failed stream");
    output_stream.write(
        reinterpret_cast<const char*>(file_bytes.data()),
        static_cast<std::streamsize>(file_bytes.size())
    );
    assert(output_stream.good() && "Expected output file write to succeed, got a failed stream");
}

/** Creates local audio assets for the TUI. side-effects: writes WAV files into the runtime directory. */
AudioAssetPaths createAudioAssets() {
    const char* runtime_directory_environment = std::getenv("XDG_RUNTIME_DIR");
    const std::string runtime_directory =
        (runtime_directory_environment != nullptr) ? std::string(runtime_directory_environment) : "/tmp";
    assert(!runtime_directory.empty() && "Expected a non-empty runtime directory, got an empty directory");

    const std::string base_path_prefix = runtime_directory + "/word_typing_practice_tui_";
    const AudioAssetPaths audio_asset_paths {
        base_path_prefix + "keystroke.wav",
        base_path_prefix + "mistake.wav",
        base_path_prefix + "success.wav",
        base_path_prefix + "new_record.wav",
    };

    writeBinaryFile(audio_asset_paths.keystroke_wav_path, createSineWaveWavBytes(22050, 950.0, 0.03, 0.08));
    writeBinaryFile(audio_asset_paths.mistake_wav_path, createSineWaveWavBytes(22050, 180.0, 0.20, 0.30));
    writeBinaryFile(audio_asset_paths.success_wav_path, createSineWaveWavBytes(22050, 700.0, 0.12, 0.20));
    writeBinaryFile(audio_asset_paths.new_record_wav_path, createSineWaveWavBytes(22050, 1040.0, 0.18, 0.24));

    return audio_asset_paths;
}

/** Converts LF line endings to terminal-safe CRLF line endings. */
std::string toTerminalLines(const std::string& frame_text) {
    std::string converted_text;
    converted_text.reserve(frame_text.size() * 2U);

    for (const char frame_character : frame_text) {
        if (frame_character == '\n') {
            converted_text += "\r\n";
        } else {
            converted_text.push_back(frame_character);
        }
    }

    return converted_text;
}

/** Returns the median value of the input vector. Input shape: (value_count). */
double calculateMedian(const std::vector<double>& values) {
    if (values.empty()) {
        return 0.0;
    }

    std::vector<double> sorted_values = values;
    std::sort(sorted_values.begin(), sorted_values.end());
    const std::size_t middle_index = sorted_values.size() / 2;

    if (sorted_values.size() % 2U == 0U) {
        return (sorted_values[middle_index - 1] + sorted_values[middle_index]) / 2.0;
    }

    return sorted_values[middle_index];
}

/** Formats a floating-point value with fixed precision. */
std::string formatFixed(const double value, const int precision) {
    std::ostringstream output_stream;
    output_stream << std::fixed << std::setprecision(precision) << value;
    return output_stream.str();
}

/** Returns an ANSI-styled version of the input text. */
std::string withStyle(const std::string_view prefix_codes, const std::string& text) {
    return std::string(prefix_codes) + text + std::string(ANSI_RESET);
}

/** Returns a human-readable label for the mistake action. */
std::string getMistakeActionLabel(const MistakeAction mistake_action) {
    switch (mistake_action) {
        case MistakeAction::Nothing:
            return "Nothing (let user backspace)";
        case MistakeAction::Prevent:
            return "Don't add wrong letter";
        case MistakeAction::Erase:
            return "Erase all text";
    }

    assert(false && "Expected a supported mistake action, got an unknown mistake action");
    std::abort();
}

/** Terminal session owner for raw-mode rendering. */
class TerminalSession {
public:
    /** Initializes the terminal session. side-effects: switches stdin to raw mode and the terminal to an alternate screen. */
    TerminalSession() {
        const int get_attributes_result = tcgetattr(STDIN_FILENO, &original_attributes_);
        assert(get_attributes_result == 0 && "Expected tcgetattr to succeed, got a failure");

        original_file_status_flags_ = fcntl(STDIN_FILENO, F_GETFL);
        assert(original_file_status_flags_ != -1 && "Expected fcntl(F_GETFL) to succeed, got -1");

        termios raw_attributes = original_attributes_;
        cfmakeraw(&raw_attributes);
        const int set_attributes_result = tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw_attributes);
        assert(set_attributes_result == 0 && "Expected tcsetattr to succeed, got a failure");

        const int set_flags_result = fcntl(STDIN_FILENO, F_SETFL, original_file_status_flags_ | O_NONBLOCK);
        assert(set_flags_result != -1 && "Expected fcntl(F_SETFL) to succeed, got -1");

        std::cout << ANSI_ALT_SCREEN_ON << ANSI_HIDE_CURSOR << ANSI_CLEAR_SCREEN << std::flush;
    }

    /** Restores the terminal session. side-effects: restores terminal settings and screen state. */
    ~TerminalSession() {
        const int restore_attributes_result = tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_attributes_);
        assert(restore_attributes_result == 0 && "Expected tcsetattr restore to succeed, got a failure");

        const int restore_flags_result = fcntl(STDIN_FILENO, F_SETFL, original_file_status_flags_);
        assert(restore_flags_result != -1 && "Expected fcntl flag restore to succeed, got -1");

        std::cout << ANSI_RESET << ANSI_SHOW_CURSOR << ANSI_ALT_SCREEN_OFF << std::flush;
    }

    /** Polls stdin for raw bytes. Return shape: (byte_count). side-effects: reads from stdin. */
    std::string pollInputBytes(const int timeout_milliseconds) const {
        assert(
            timeout_milliseconds >= 0 &&
            "Expected a non-negative poll timeout, got a negative timeout"
        );

        fd_set read_file_descriptors;
        FD_ZERO(&read_file_descriptors);
        FD_SET(STDIN_FILENO, &read_file_descriptors);

        timeval timeout {};
        timeout.tv_sec = timeout_milliseconds / 1000;
        timeout.tv_usec = (timeout_milliseconds % 1000) * 1000;

        const int select_result = select(
            STDIN_FILENO + 1,
            &read_file_descriptors,
            nullptr,
            nullptr,
            &timeout
        );
        assert(select_result != -1 && "Expected select to succeed, got -1");

        if (select_result == 0) {
            return "";
        }

        std::array<char, 256> read_buffer {};
        const ssize_t bytes_read = read(STDIN_FILENO, read_buffer.data(), read_buffer.size());
        if (bytes_read == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return "";
        }
        assert(bytes_read != -1 && "Expected read to succeed, got -1");
        return std::string(read_buffer.data(), static_cast<std::size_t>(bytes_read));
    }

    /** Writes a full-screen frame. side-effects: writes to stdout. */
    void drawFrame(const std::string& frame_text) const {
        std::cout << ANSI_CLEAR_SCREEN << frame_text << std::flush;
    }

private:
    termios original_attributes_ {};
    int original_file_status_flags_ = 0;
};

/** Interactive word typing practice application. */
class Application {
public:
    /** Runs the application loop. side-effects: reads keyboard input, writes terminal frames, and emits terminal bell sounds. */
    void run() {
        TerminalSession terminal_session;

        while (is_running_) {
            const std::string raw_input_bytes = terminal_session.pollInputBytes(FRAME_TIMEOUT_MS);
            processRawInputBytes(raw_input_bytes);
            refreshFlashState();
            terminal_session.drawFrame(renderFrame());
        }
    }

private:
    /** Returns the WAV path for a sound effect. side-effects: none. */
    std::string getSoundEffectPath(const SoundEffect sound_effect) const {
        switch (sound_effect) {
            case SoundEffect::Keystroke:
                return audio_asset_paths_.keystroke_wav_path;
            case SoundEffect::Mistake:
                return audio_asset_paths_.mistake_wav_path;
            case SoundEffect::Success:
                return audio_asset_paths_.success_wav_path;
            case SoundEffect::NewRecord:
                return audio_asset_paths_.new_record_wav_path;
        }

        assert(false && "Expected a supported sound effect, got an unknown sound effect");
        std::abort();
    }

    /** Launches one sound playback command in the background. side-effects: spawns a child shell command. */
    void playSoundEffect(const SoundEffect sound_effect) {
        if (!sound_enabled_) {
            return;
        }
        if (audio_backend_.backend_type == AudioBackendType::None) {
            return;
        }

        const std::string wav_path = getSoundEffectPath(sound_effect);
        std::string command_text;

        switch (audio_backend_.backend_type) {
            case AudioBackendType::Paplay:
                command_text =
                    quoteForShell(audio_backend_.executable_path) + " " + quoteForShell(wav_path) +
                    " >/dev/null 2>&1 &";
                break;
            case AudioBackendType::Aplay:
                command_text =
                    quoteForShell(audio_backend_.executable_path) + " -q " + quoteForShell(wav_path) +
                    " >/dev/null 2>&1 &";
                break;
            case AudioBackendType::Ffplay:
                command_text =
                    quoteForShell(audio_backend_.executable_path) +
                    " -nodisp -autoexit -loglevel quiet " + quoteForShell(wav_path) +
                    " >/dev/null 2>&1 &";
                break;
            case AudioBackendType::None:
                return;
        }

        const int system_result = std::system(command_text.c_str());
        assert(system_result != -1 && "Expected std::system to succeed, got -1");
    }

    /** Returns the effective practice word. side-effects: none. */
    std::string getEffectiveTargetWord() const {
        return configured_word_utf8_;
    }

    /** Resets the timer. side-effects: mutates timer state. */
    void resetTimer() {
        start_time_.reset();
    }

    /** Returns the current elapsed time in seconds. side-effects: none. */
    double getElapsedSeconds() const {
        if (!start_time_.has_value()) {
            return 0.0;
        }

        const auto elapsed_duration = Clock::now() - *start_time_;
        const auto elapsed_milliseconds =
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed_duration).count();
        return static_cast<double>(elapsed_milliseconds) / 1000.0;
    }

    /** Clears current practice input and visual state after a word edit. side-effects: mutates practice input and timer state. */
    void resetPracticeStateForWordChange() {
        practice_input_utf8_.clear();
        flash_kind_ = FlashKind::None;
        flash_deadline_.reset();
        resetTimer();
    }

    /** Resets all statistics after a word edit. side-effects: mutates statistics. */
    void resetStatisticsForWordChange() {
        statistics_ = Statistics {};
    }

    /** Applies a target-word edit character. side-effects: mutates the configured word, practice input, timer, and statistics. */
    void appendWordCharacter(const std::string& utf8_text) {
        configured_word_utf8_ += utf8_text;
        resetPracticeStateForWordChange();
        resetStatisticsForWordChange();
    }

    /** Applies a target-word backspace. side-effects: mutates the configured word, practice input, timer, and statistics. */
    void eraseWordCharacter() {
        popLastUtf8CodePoint(configured_word_utf8_);
        resetPracticeStateForWordChange();
        resetStatisticsForWordChange();
    }

    /** Starts the current flash state. side-effects: mutates flash state. */
    void startFlash(const FlashKind flash_kind) {
        flash_kind_ = flash_kind;
        flash_deadline_ = Clock::now() + std::chrono::milliseconds(FLASH_DURATION_MS);
    }

    /** Expires flashes whose deadline has passed. side-effects: mutates flash state. */
    void refreshFlashState() {
        if (!flash_deadline_.has_value()) {
            return;
        }

        if (Clock::now() >= *flash_deadline_) {
            flash_kind_ = FlashKind::None;
            flash_deadline_.reset();
        }
    }

    /** Cycles the configured mistake action. side-effects: mutates mistake action. */
    void cycleMistakeAction() {
        switch (mistake_action_) {
            case MistakeAction::Nothing:
                mistake_action_ = MistakeAction::Prevent;
                return;
            case MistakeAction::Prevent:
                mistake_action_ = MistakeAction::Erase;
                return;
            case MistakeAction::Erase:
                mistake_action_ = MistakeAction::Nothing;
                return;
        }

        assert(false && "Expected a supported mistake action, got an unknown mistake action");
        std::abort();
    }

    /** Processes a changed practice buffer exactly like the browser app. side-effects: mutates practice state, timer, statistics, flash state, and may emit sound. */
    void handlePracticeBufferChanged() {
        const std::string target_word_utf8 = getEffectiveTargetWord();
        const std::size_t current_input_length = countUtf8CodePoints(practice_input_utf8_);

        if (current_input_length == 1U && !start_time_.has_value()) {
            start_time_ = Clock::now();
        }

        const bool is_correct_so_far = startsWithBytes(target_word_utf8, practice_input_utf8_);

        if (!is_correct_so_far && !practice_input_utf8_.empty()) {
            ++statistics_.mistake_count;

            if (mistake_feedback_enabled_) {
                playSoundEffect(SoundEffect::Mistake);
                startFlash(FlashKind::Error);
            } else {
                playSoundEffect(SoundEffect::Keystroke);
                flash_kind_ = FlashKind::None;
                flash_deadline_.reset();
            }

            switch (mistake_action_) {
                case MistakeAction::Nothing:
                    break;
                case MistakeAction::Prevent:
                    popLastUtf8CodePoint(practice_input_utf8_);
                    break;
                case MistakeAction::Erase:
                    practice_input_utf8_.clear();
                    ++statistics_.word_attempts;
                    resetTimer();
                    break;
            }

            return;
        }

        if (!practice_input_utf8_.empty()) {
            ++statistics_.correct_count;
            playSoundEffect(SoundEffect::Keystroke);
        }

        if (practice_input_utf8_ == target_word_utf8) {
            registerSuccessfulCompletion();
            return;
        }

        flash_kind_ = FlashKind::None;
        flash_deadline_.reset();
    }

    /** Registers a successful word completion. side-effects: mutates statistics, timer, input buffer, flash state, and may emit sound. */
    void registerSuccessfulCompletion() {
        assert(start_time_.has_value() && "Expected a started timer before completion, got no timer");

        const double completion_time_seconds = getElapsedSeconds();
        assert(
            completion_time_seconds > 0.0 &&
            "Expected a positive completion time, got a non-positive completion time"
        );

        statistics_.completion_times_seconds.push_back(completion_time_seconds);

        const double best_time_seconds = *std::min_element(
            statistics_.completion_times_seconds.begin(),
            statistics_.completion_times_seconds.end()
        );
        const bool is_new_record = completion_time_seconds == best_time_seconds;

        if (is_new_record) {
            playSoundEffect(SoundEffect::NewRecord);
            startFlash(FlashKind::NewRecord);
        } else {
            playSoundEffect(SoundEffect::Success);
            startFlash(FlashKind::Success);
        }

        ++statistics_.words_completed;
        ++statistics_.word_attempts;
        practice_input_utf8_.clear();
        resetTimer();
    }

    /** Appends a practice character and re-runs browser-equivalent validation. side-effects: mutates practice state, timer, statistics, and may emit sound. */
    void appendPracticeCharacter(const std::string& utf8_text) {
        practice_input_utf8_ += utf8_text;
        handlePracticeBufferChanged();
    }

    /** Applies a practice backspace and re-runs browser-equivalent validation. side-effects: mutates practice state, timer, statistics, and may emit sound. */
    void erasePracticeCharacter() {
        popLastUtf8CodePoint(practice_input_utf8_);
        handlePracticeBufferChanged();
    }

    /** Consumes one decoded input event. side-effects: mutates application state and may emit sound. */
    void handleInputEvent(const InputEvent& input_event) {
        switch (input_event.type) {
            case InputEventType::None:
                return;
            case InputEventType::Character:
                if (focus_mode_ == FocusMode::EditWord) {
                    appendWordCharacter(input_event.utf8_text);
                } else {
                    appendPracticeCharacter(input_event.utf8_text);
                }
                return;
            case InputEventType::Backspace:
                if (focus_mode_ == FocusMode::EditWord) {
                    eraseWordCharacter();
                } else {
                    erasePracticeCharacter();
                }
                return;
            case InputEventType::ToggleFocus:
                focus_mode_ = (focus_mode_ == FocusMode::Practice) ? FocusMode::EditWord : FocusMode::Practice;
                return;
            case InputEventType::CycleMistakeAction:
                cycleMistakeAction();
                return;
            case InputEventType::ToggleSound:
                sound_enabled_ = !sound_enabled_;
                return;
            case InputEventType::ToggleMistakeFeedback:
                mistake_feedback_enabled_ = !mistake_feedback_enabled_;
                return;
            case InputEventType::ToggleHideText:
                hide_text_ = !hide_text_;
                return;
            case InputEventType::Quit:
                is_running_ = false;
                return;
        }

        assert(false && "Expected a supported input event type, got an unknown input event type");
        std::abort();
    }

    /** Decodes one raw byte into zero or one input events. Return shape: (0 or 1). side-effects: mutates decoder state. */
    std::optional<InputEvent> decodeInputByte(const unsigned char input_byte) {
        if (escape_sequence_active_) {
            if (input_byte >= 0x40U && input_byte <= 0x7EU) {
                escape_sequence_active_ = false;
            }
            return std::nullopt;
        }

        if (!pending_utf8_bytes_.empty()) {
            assert(
                isUtf8ContinuationByte(input_byte) &&
                "Expected a UTF-8 continuation byte, got a non-continuation byte"
            );
            pending_utf8_bytes_.push_back(static_cast<char>(input_byte));
            if (pending_utf8_bytes_.size() == pending_utf8_length_) {
                InputEvent character_event;
                character_event.type = InputEventType::Character;
                character_event.utf8_text = pending_utf8_bytes_;
                pending_utf8_bytes_.clear();
                pending_utf8_length_ = 0;
                return character_event;
            }
            return std::nullopt;
        }

        if (input_byte == 27U) {
            escape_sequence_active_ = true;
            return std::nullopt;
        }
        if (input_byte == 3U) {
            return InputEvent {InputEventType::Quit, ""};
        }
        if (input_byte == 9U) {
            return InputEvent {InputEventType::ToggleFocus, ""};
        }
        if (input_byte == 1U) {
            return InputEvent {InputEventType::CycleMistakeAction, ""};
        }
        if (input_byte == 19U) {
            return InputEvent {InputEventType::ToggleSound, ""};
        }
        if (input_byte == 6U) {
            return InputEvent {InputEventType::ToggleMistakeFeedback, ""};
        }
        if (input_byte == 20U) {
            return InputEvent {InputEventType::ToggleHideText, ""};
        }
        if (input_byte == 8U || input_byte == 127U) {
            return InputEvent {InputEventType::Backspace, ""};
        }
        if (input_byte == 10U || input_byte == 13U) {
            return std::nullopt;
        }
        if (input_byte < 32U) {
            return std::nullopt;
        }

        const std::size_t expected_sequence_length = getExpectedUtf8SequenceLength(input_byte);
        if (expected_sequence_length == 1U) {
            return InputEvent {InputEventType::Character, std::string(1, static_cast<char>(input_byte))};
        }

        pending_utf8_bytes_ = std::string(1, static_cast<char>(input_byte));
        pending_utf8_length_ = expected_sequence_length;
        return std::nullopt;
    }

    /** Processes all bytes collected in the latest polling step. side-effects: mutates application and decoder state. */
    void processRawInputBytes(const std::string& raw_input_bytes) {
        // Loop invariant: each earlier byte has already been decoded and applied exactly once.
        for (const unsigned char input_byte : rawInputToUnsignedBytes(raw_input_bytes)) {
            const std::optional<InputEvent> maybe_input_event = decodeInputByte(input_byte);
            if (maybe_input_event.has_value()) {
                handleInputEvent(*maybe_input_event);
            }
        }
    }

    /** Converts raw input bytes to unsigned bytes. Return shape: (byte_count). */
    std::vector<unsigned char> rawInputToUnsignedBytes(const std::string& raw_input_bytes) const {
        std::vector<unsigned char> unsigned_bytes;
        unsigned_bytes.reserve(raw_input_bytes.size());
        for (const char input_byte : raw_input_bytes) {
            unsigned_bytes.push_back(static_cast<unsigned char>(input_byte));
        }
        return unsigned_bytes;
    }

    /** Renders the target-word highlight line. side-effects: none. */
    std::string renderWordDisplay() const {
        if (hide_text_) {
            return withStyle(ANSI_DIM, "[hidden]");
        }

        const std::vector<std::string> target_code_points = splitUtf8CodePoints(getEffectiveTargetWord());
        const std::vector<std::string> input_code_points = splitUtf8CodePoints(practice_input_utf8_);
        std::ostringstream output_stream;

        if (mistake_action_ == MistakeAction::Nothing) {
            std::size_t target_index = 0;

            // Loop invariant: `target_index` equals the number of correct code points consumed so far.
            for (const std::string& typed_code_point : input_code_points) {
                const bool has_expected_code_point = target_index < target_code_points.size();
                const bool is_matching_code_point =
                    has_expected_code_point && typed_code_point == target_code_points[target_index];

                if (is_matching_code_point) {
                    output_stream << withStyle(std::string(ANSI_WHITE) + std::string(ANSI_BOLD), typed_code_point);
                    ++target_index;
                } else {
                    output_stream << withStyle(
                        std::string(ANSI_RED) + std::string(ANSI_STRIKETHROUGH),
                        typed_code_point
                    );
                }
            }

            if (target_index < target_code_points.size()) {
                output_stream << withStyle(
                    std::string(ANSI_YELLOW) + std::string(ANSI_UNDERLINE),
                    target_code_points[target_index]
                );
                ++target_index;
            }

            for (; target_index < target_code_points.size(); ++target_index) {
                output_stream << withStyle(std::string(ANSI_DIM), target_code_points[target_index]);
            }

            return output_stream.str();
        }

        std::size_t correct_prefix_length = 0;
        while (
            correct_prefix_length < target_code_points.size() &&
            correct_prefix_length < input_code_points.size() &&
            target_code_points[correct_prefix_length] == input_code_points[correct_prefix_length]
        ) {
            ++correct_prefix_length;
        }

        // Loop invariant: every code point before `code_point_index` has already been rendered exactly once.
        for (std::size_t code_point_index = 0; code_point_index < target_code_points.size(); ++code_point_index) {
            if (code_point_index < correct_prefix_length) {
                output_stream << withStyle(
                    std::string(ANSI_WHITE) + std::string(ANSI_BOLD),
                    target_code_points[code_point_index]
                );
            } else if (code_point_index == correct_prefix_length) {
                output_stream << withStyle(
                    std::string(ANSI_YELLOW) + std::string(ANSI_UNDERLINE),
                    target_code_points[code_point_index]
                );
            } else {
                output_stream << withStyle(std::string(ANSI_DIM), target_code_points[code_point_index]);
            }
        }

        return output_stream.str();
    }

    /** Returns the current input-status label. side-effects: none. */
    std::string renderInputStatusLabel() const {
        switch (flash_kind_) {
            case FlashKind::None:
                return "Idle";
            case FlashKind::Error:
                return "Mistake";
            case FlashKind::Success:
                return "Success";
            case FlashKind::NewRecord:
                return "New record";
        }

        assert(false && "Expected a supported flash kind, got an unknown flash kind");
        std::abort();
    }

    /** Returns the ANSI prefix for the input-status label. side-effects: none. */
    std::string getInputStatusStyle() const {
        switch (flash_kind_) {
            case FlashKind::None:
                return std::string(ANSI_CYAN);
            case FlashKind::Error:
                return std::string(ANSI_BG_RED);
            case FlashKind::Success:
            case FlashKind::NewRecord:
                return std::string(ANSI_BG_GREEN);
        }

        assert(false && "Expected a supported flash kind, got an unknown flash kind");
        std::abort();
    }

    /** Returns a yes-or-no label. side-effects: none. */
    std::string getBooleanLabel(const bool flag_value) const {
        return flag_value ? "on" : "off";
    }

    /** Returns the active word editor display. side-effects: none. */
    std::string renderConfiguredWord() const {
        if (hide_text_ && !configured_word_utf8_.empty()) {
            return maskUtf8Text(configured_word_utf8_);
        }
        return configured_word_utf8_;
    }

    /** Returns the completion-rate percentage. side-effects: none. */
    int getCompletionRatePercent() const {
        if (statistics_.word_attempts == 0U) {
            return 100;
        }
        const double ratio =
            static_cast<double>(statistics_.words_completed) / static_cast<double>(statistics_.word_attempts);
        return static_cast<int>(std::llround(ratio * 100.0));
    }

    /** Returns the accuracy percentage. side-effects: none. */
    int getAccuracyPercent() const {
        const std::uint64_t total_inputs = statistics_.correct_count + statistics_.mistake_count;
        if (total_inputs == 0U) {
            return 100;
        }
        const double ratio = static_cast<double>(statistics_.correct_count) / static_cast<double>(total_inputs);
        return static_cast<int>(std::llround(ratio * 100.0));
    }

    /** Renders one timing-stat field. side-effects: none. */
    std::string renderTimingStats() const {
        if (statistics_.completion_times_seconds.empty() || getEffectiveTargetWord().empty()) {
            return "Best: - (-) | Median: - (-) | Last: - (-)";
        }

        const double word_length = static_cast<double>(countUtf8CodePoints(getEffectiveTargetWord()));
        assert(word_length > 0.0 && "Expected a positive word length, got a non-positive word length");

        std::vector<double> speeds_symbols_per_second;
        speeds_symbols_per_second.reserve(statistics_.completion_times_seconds.size());
        for (const double completion_time_seconds : statistics_.completion_times_seconds) {
            speeds_symbols_per_second.push_back(word_length / completion_time_seconds);
        }

        const double best_speed = *std::max_element(
            speeds_symbols_per_second.begin(),
            speeds_symbols_per_second.end()
        );
        const double median_speed = calculateMedian(speeds_symbols_per_second);
        const double best_time = *std::min_element(
            statistics_.completion_times_seconds.begin(),
            statistics_.completion_times_seconds.end()
        );
        const double median_time = calculateMedian(statistics_.completion_times_seconds);
        const double last_time = statistics_.completion_times_seconds.back();
        const double last_speed = word_length / last_time;

        std::ostringstream output_stream;
        output_stream
            << "Best: " << formatFixed(best_speed, 1) << " sym/s (" << formatFixed(best_time, 2) << "s)"
            << " | Median: " << formatFixed(median_speed, 1) << " sym/s (" << formatFixed(median_time, 2) << "s)"
            << " | Last: " << formatFixed(last_speed, 1) << " sym/s (" << formatFixed(last_time, 2) << "s)";
        return output_stream.str();
    }

    /** Renders the full screen frame. side-effects: none. */
    std::string renderFrame() const {
        const std::string focus_label =
            (focus_mode_ == FocusMode::Practice) ? "Practice input" : "Word editor";
        const std::string focus_style =
            (focus_mode_ == FocusMode::Practice) ? std::string(ANSI_BG_GREEN) : std::string(ANSI_BG_RED);
        const std::string sound_label =
            (audio_backend_.backend_type == AudioBackendType::None)
                ? (getBooleanLabel(sound_enabled_) + " (no backend)")
                : getBooleanLabel(sound_enabled_);

        std::ostringstream output_stream;
        output_stream << withStyle(std::string(ANSI_BOLD), "Word Typing Practice (C++ TUI)") << "\n";
        output_stream << "Controls: `Tab` switch focus | `Ctrl+A` action | `Ctrl+S` sound | "
                      << "`Ctrl+F` feedback | `Ctrl+T` hide text | `Ctrl+C` quit\n";
        output_stream << "Focus: " << withStyle(focus_style, focus_label) << "\n\n";

        output_stream << "Word to Practice: " << renderConfiguredWord() << "\n";
        output_stream << "Action on Mistake: " << getMistakeActionLabel(mistake_action_) << "\n";
        output_stream << "Sound: " << sound_label
                      << " | Mistake feedback: " << getBooleanLabel(mistake_feedback_enabled_)
                      << " | Hide text: " << getBooleanLabel(hide_text_) << "\n\n";

        output_stream << "Target: " << renderWordDisplay() << "\n";
        output_stream << "Timer: " << formatFixed(getElapsedSeconds(), 2) << "s\n";
        output_stream << "Status: " << withStyle(getInputStatusStyle(), renderInputStatusLabel()) << "\n\n";

        output_stream << "Words Completed: " << statistics_.words_completed
                      << " | Completion Rate: " << getCompletionRatePercent() << "%\n";
        output_stream << renderTimingStats() << "\n";
        output_stream << "Correct: " << statistics_.correct_count
                      << " | Mistakes: " << statistics_.mistake_count
                      << " | Accuracy: " << getAccuracyPercent() << "%\n";

        return toTerminalLines(output_stream.str());
    }

    AudioBackend audio_backend_ = detectAudioBackend();
    AudioAssetPaths audio_asset_paths_ = createAudioAssets();
    std::string configured_word_utf8_ = std::string(DEFAULT_WORD);
    std::string practice_input_utf8_;
    Statistics statistics_;
    MistakeAction mistake_action_ = MistakeAction::Erase;
    FocusMode focus_mode_ = FocusMode::Practice;
    FlashKind flash_kind_ = FlashKind::None;
    bool sound_enabled_ = false;
    bool mistake_feedback_enabled_ = true;
    bool hide_text_ = false;
    bool is_running_ = true;
    bool escape_sequence_active_ = false;
    std::size_t pending_utf8_length_ = 0;
    std::string pending_utf8_bytes_;
    std::optional<TimePoint> start_time_;
    std::optional<TimePoint> flash_deadline_;
};

}  // namespace

/** Entry point for the application. side-effects: runs an interactive terminal application. */
int main() {
    Application application;
    application.run();
    return 0;
}
