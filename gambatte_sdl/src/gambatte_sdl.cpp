/***************************************************************************
 *   Copyright (C) 2007 by Sindre Aam�s                                    *
 *   sinamas@users.sourceforge.net                                         *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License version 2 as     *
 *   published by the Free Software Foundation.                            *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License version 2 for more details.                *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   version 2 along with this program; if not, write to the               *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/
#include "array.h"
#include "audiodata.h"
#include "blitterwrapper.h"
#include "parser.h"
#include "rateest.h"
#include "resample/resamplerinfo.h"
#include "skipsched.h"
#include "str_to_sdlkey.h"
#include "syncfunc.h"
#include "videolink/vfilterinfo.h"
#include "videolink/videolink.h"
#include <gambatte.h>
#include <pakinfo.h>
#include <SDL.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace gambatte;

struct DescOption : Parser::Option {
	explicit DescOption(const char *s, char c = 0, int nArgs = 0)
	: Option(s, c, nArgs)
	{
	}

	virtual ~DescOption() {}
	virtual const char* desc() const = 0;
};

class BoolOption : public DescOption {
	char const *const desc_;
	bool isSet_;
public:
	BoolOption(char const *desc, char const *s, char c = 0)
	: DescOption(s, c), desc_(desc), isSet_(false)
	{
	}

	virtual void exec(char const *const *, int) { isSet_ = true; }
	virtual char const * desc() const { return desc_; }
	bool isSet() const { return isSet_; }
};

class RateOption : public DescOption {
	unsigned rate;
public:
	RateOption() : DescOption("sample-rate", 'r', 1), rate(48000) {}

	void exec(const char *const *argv, int index) {
		int tmp = std::atoi(argv[index + 1]);

		if (tmp < 4000 || tmp > 192000)
			return;

		rate = tmp;
	}

	const char* desc() const { return " N\t\tUse audio sample rate of N Hz\n\t\t\t\t    4000 <= N <= 192000, default: 48000\n"; }
	unsigned getRate() const { return rate; }
};

class LatencyOption : public DescOption {
	unsigned latency;
public:
	LatencyOption() : DescOption("latency", 'l', 1), latency(133) {}

	void exec(const char *const *argv, int index) {
		int tmp = std::atoi(argv[index + 1]);

		if (tmp < 16 || tmp > 5000)
			return;

		latency = tmp;
	}

	const char* desc() const { return " N\t\tUse audio buffer latency of N ms\n\t\t\t\t    16 <= N <= 5000, default: 133\n"; }
	unsigned getLatency() const { return latency; }
};

class PeriodsOption : public DescOption {
	unsigned periods;
public:
	PeriodsOption() : DescOption("periods", 'p', 1), periods(4) {}

	void exec(const char *const *argv, int index) {
		int tmp = std::atoi(argv[index + 1]);

		if (tmp < 1 || tmp > 32)
			return;

		periods = tmp;
	}

	const char* desc() const { return " N\t\tUse N audio buffer periods\n\t\t\t\t    1 <= N <= 32, default: 4\n"; }
	unsigned getPeriods() const { return periods; }
};

class ScaleOption : public DescOption {
	int scale_;

public:
	ScaleOption() : DescOption("scale", 's', 1), scale_(1) {}

	void exec(const char *const *argv, int index) {
		int s = std::atoi(argv[index + 1]);
		if (s < 1 || s > 40)
			return;

		scale_ = s;
	}

	const char* desc() const { return " N\t\t\tScale video output by an integer factor of N\n"; }
	int scale() const { return scale_; }
};

class VfOption : public DescOption {
	std::string s;
	unsigned filterNr;
public:
	VfOption();
	void exec(const char *const *argv, int index) {
		filterNr = std::min<unsigned>(std::max(std::atoi(argv[index + 1]), 0), VfilterInfo::numVfilters() - 1);
	}
	const char* desc() const { return s.c_str(); }
	unsigned filterNumber() const { return filterNr; }
};

VfOption::VfOption() : DescOption("video-filter", 'v', 1), filterNr(0) {
	std::stringstream ss;
	ss << " N\t\tUse video filter number N\n";

	for (std::size_t i = 0; i < VfilterInfo::numVfilters(); ++i) {
		ss << "\t\t\t\t    " << i << " = " << VfilterInfo::get(i).handle << "\n";
	}

	s = ss.str();
}

class ResamplerOption : public DescOption {
	std::string s;
	unsigned resamplerNo;
public:
	ResamplerOption();

	void exec(const char *const *argv, int index) {
		const unsigned tmp = std::atoi(argv[index + 1]);

		if (tmp < ResamplerInfo::num())
			resamplerNo = tmp;
	}

	const char* desc() const { return s.c_str(); }
	unsigned resamplerNumber() const { return resamplerNo; }
};

ResamplerOption::ResamplerOption() : DescOption("resampler", 0, 1), resamplerNo(1) {
	std::stringstream ss;
	ss << " N\t\tUse audio resampler number N\n";

	for (std::size_t i = 0; i < ResamplerInfo::num(); ++i) {
		ss << "\t\t\t\t    " << i << " = " << ResamplerInfo::get(i).desc;

		if (i == resamplerNo)
			ss << " [default]";

		ss << "\n";
	}

	s = ss.str();
}

struct JoyData {
	enum { CENTERED = SDL_HAT_CENTERED, LEFT = SDL_HAT_LEFT, RIGHT = SDL_HAT_RIGHT, UP = SDL_HAT_UP, DOWN = SDL_HAT_DOWN };

	union {
		struct {
			Uint8 dev_num;
			Uint8 num;
		};

		Uint16 id;
	};

	Sint16 dir;
};

static inline bool operator<(const JoyData &l, const JoyData &r) {
	return l.id < r.id;
}

class InputOption : public DescOption {
public:
	struct InputId {
		enum { KEY, JOYBUT, JOYAX, JOYHAT } type;

		union {
			JoyData jdata;
			SDLKey key;
		};

		InputId() : type(KEY) {}
	};

private:
	InputId keys[8];

public:
	InputOption();
	void exec(const char *const *argv, int index);
	const char* desc() const { return " KEYS\t\tUse the 8 given input KEYS for respectively\n\t\t\t\t    START SELECT A B UP DOWN LEFT RIGHT\n"; }
	const InputId* getKeys() const { return keys; }
};

InputOption::InputOption() : DescOption("input", 'i', 8) {
	keys[0].key = SDLK_RETURN;
	keys[1].key = SDLK_RSHIFT;
	keys[2].key = SDLK_d;
	keys[3].key = SDLK_c;
	keys[4].key = SDLK_UP;
	keys[5].key = SDLK_DOWN;
	keys[6].key = SDLK_LEFT;
	keys[7].key = SDLK_RIGHT;
}

void InputOption::exec(const char *const *argv, int index) {
	++index;

	for (unsigned i = 0; i < 8; ++i) {
		const char *s = argv[index + i];

		if (s[0] == 'j' && s[1] == 's') {
			s += 2;
			const char *const send = s + std::strlen(s);

			if (send - s < 3)
				continue;

			const int dev_num = std::atoi(s++);

			if (dev_num < 0 || dev_num > 255)
				continue;

			s += (dev_num > 9) + (dev_num > 99);

			if (send - s < 2)
				continue;

			const char type = *s++;
			const int num = std::atoi(s++);

			if (num < 0 || num > 255)
				continue;

			s += (dev_num > 9) + (dev_num > 99);

			InputId id;
			id.jdata.dev_num = dev_num;
			id.jdata.num = num;

			if (type == 'b') {
				if (send - s != 0)
					continue;

				id.type = InputId::JOYBUT;
			} else {
				if (send - s != 1)
					continue;

				const char dir = *s;

				switch (type) {
				case 'a':
					switch (dir) {
					case '+': id.jdata.dir = JoyData::UP; break;
					case '-': id.jdata.dir = JoyData::DOWN; break;
					default: continue;
					}

					id.type = InputId::JOYAX;
					break;
				case 'h':
					switch (dir) {
					case 'u': id.jdata.dir = JoyData::UP;
					case 'd': id.jdata.dir = JoyData::DOWN;
					case 'l': id.jdata.dir = JoyData::LEFT;
					case 'r': id.jdata.dir = JoyData::RIGHT;
					default: continue;
					}

					id.type = InputId::JOYHAT;
					break;
				default: continue;
				}
			}

			keys[i] = id;
		} else if (SDLKey const *const k = strToSdlkey(s)) {
			keys[i].type = InputId::KEY;
			keys[i].key = *k;
		}
	}
}

class GetInput : public InputGetter {
public:
	unsigned is;

	GetInput() : is(0) {}
	unsigned operator()() { return is; }
};

class SdlIniter {
	bool failed;
public:
	SdlIniter();
	~SdlIniter();
	bool isFailed() const { return failed; }
};

SdlIniter::SdlIniter() : failed(false) {
	if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_VIDEO) < 0) {
		std::fprintf(stderr, "Unable to init SDL: %s\n", SDL_GetError());
		failed = true;
	}
}

SdlIniter::~SdlIniter() {
	SDL_Quit();
}

class GambatteSdl {
	typedef std::multimap<SDLKey,unsigned> keymap_t;
	typedef std::multimap<JoyData,unsigned> jmap_t;

	Array<Sint16> inBuf;
	Array<Sint16> tmpBuf;
	GB gambatte;
	GetInput inputGetter;
	SkipSched skipSched;
	keymap_t keyMap;
	jmap_t jbMap;
	jmap_t jaMap;
	jmap_t jhMap;
	SdlIniter sdlIniter;
	scoped_ptr<Resampler> resampler;
	scoped_ptr<BlitterWrapper> blitter;
	std::vector<SDL_Joystick*> joysticks;
	unsigned sampleRate;
	unsigned latency;
	unsigned periods;

	bool handleEvents();

public:
	GambatteSdl();
	~GambatteSdl();
	int init(int argc, char **argv);
	int exec();
};

GambatteSdl::GambatteSdl()
: inBuf((35112 + 2064) * 2)
, sampleRate(48000)
, latency(133)
, periods(4)
{
}

GambatteSdl::~GambatteSdl() {
	for (std::size_t i = 0; i < joysticks.size(); ++i)
		SDL_JoystickClose(joysticks[i]);
}

static void printUsage(std::vector<DescOption*> const &v) {
	std::puts("Usage: gambatte_sdl [OPTION]... romfile\n");

	for (std::size_t i = 0; i < v.size(); ++i) {
		std::printf("  ");

		if (v[i]->character())
			std::printf("-%c, ", v[i]->character());
		else
			std::printf("    ");

		std::printf("--%s%s\n", v[i]->str(), v[i]->desc());
	}
}

int GambatteSdl::init(int argc, char **argv) {
	std::puts("Gambatte SDL git");

	if (sdlIniter.isFailed())
		return EXIT_FAILURE;

	std::vector<Uint8> jdevnums;
	BoolOption fsOption("\t\tStart in full screen mode\n", "full-screen", 'f');
	ScaleOption scaleOption;
	VfOption vfOption;
	BoolOption yuvOption("\t\tUse YUV overlay for (usually faster) scaling\n",
	                     "yuv-overlay", 'y');

	{
		BoolOption controlsOption("\t\tShow keyboard controls\n", "controls");
		BoolOption gbaCgbOption("\t\t\tGBA CGB mode\n", "gba-cgb");
		BoolOption forceDmgOption("\t\tForce DMG mode\n", "force-dmg");
		BoolOption multicartCompatOption(
			"\tSupport certain multicart ROM images by\n"
			"\t\t\t\tnot strictly respecting ROM header MBC type\n", "multicart-compat");
		InputOption inputOption;
		LatencyOption latencyOption;
		BoolOption lkOption("\t\tList valid input KEYS\n", "list-keys");
		PeriodsOption periodsOption;
		RateOption rateOption;
		ResamplerOption resamplerOption;

		std::vector<DescOption*> v;
		v.push_back(&controlsOption);
		v.push_back(&gbaCgbOption);
		v.push_back(&forceDmgOption);
		v.push_back(&multicartCompatOption);
		v.push_back(&fsOption);
		v.push_back(&inputOption);
		v.push_back(&latencyOption);
		v.push_back(&lkOption);
		v.push_back(&periodsOption);
		v.push_back(&rateOption);
		v.push_back(&resamplerOption);
		v.push_back(&scaleOption);
		v.push_back(&vfOption);
		v.push_back(&yuvOption);

		Parser parser;
		for (std::size_t i = 0; i < v.size(); ++i) {
			parser.add(v[i]);
		}

		unsigned loadIndex = 0;

		for (int i = 1; i < argc; ++i) {
			if (argv[i][0] == '-') {
				if (!(i = parser.parse(argc, argv, i))) {
					printUsage(v);
					return EXIT_FAILURE;
				}
			} else if (!loadIndex) {
				loadIndex = i;
			}
		}

		if (lkOption.isSet()) {
			std::puts("Valid input KEYS:");
			printStrSdlkeys();
			static char const      jsnam[] = "jsNaM";
			static char const      jsnhm[] = "jsNhM";
			static char const joystick_n[] = "Joystick N";
			static char const     axis_m[] = "axis M";
			static char const      hat_m[] = "hat M";
			std::printf("%s+\t(%s %s +)\n", jsnam, joystick_n, axis_m);
			std::printf("%s-\t(%s %s -)\n", jsnam, joystick_n, axis_m);
			std::printf("jsNbM\t(%s button M)\n", joystick_n);
			std::printf("%sd\t(%s %s down)\n", jsnhm, joystick_n, hat_m);
			std::printf("%sl\t(%s %s left)\n", jsnhm, joystick_n, hat_m);
			std::printf("%sr\t(%s %s right)\n", jsnhm, joystick_n, hat_m);
			std::printf("%su\t(%s %s up)\n", jsnhm, joystick_n, hat_m);
		}

		if (controlsOption.isSet()) {
			std::puts("Controls:");
			std::puts("TAB\t- fast-forward");
			std::puts("Ctrl-f\t- toggle full screen");
			std::puts("Ctrl-r\t- reset");
			std::puts("F5\t- save state");
			std::puts("F6\t- previous state slot");
			std::puts("F7\t- next state slot");
			std::puts("F8\t- load state");
			std::puts("0 to 9\t- select state slot 0 to 9");
			std::puts("");
			std::puts("Default key mapping:");
			std::puts("Up:\tup");
			std::puts("Down:\tdown");
			std::puts("Left:\tleft");
			std::puts("Right:\tright");
			std::puts("A:\td");
			std::puts("B:\tc");
			std::puts("Start:\treturn");
			std::puts("Select:\trshift");
		}

		if (!loadIndex) {
			if (!lkOption.isSet() && !controlsOption.isSet())
				printUsage(v);

			return 0;
		}

		if (LoadRes const error =
				gambatte.load(argv[loadIndex],
				                gbaCgbOption.isSet()          * GB::GBA_CGB
				              + forceDmgOption.isSet()        * GB::FORCE_DMG
				              + multicartCompatOption.isSet() * GB::MULTICART_COMPAT)) {
			std::printf("failed to load ROM %s: %s\n", argv[loadIndex], to_string(error).c_str());
			return error;
		}

		{
			PakInfo const &pak = gambatte.pakInfo();
			std::puts(gambatte.romTitle().c_str());
			std::printf("GamePak type: %s rambanks: %u rombanks: %u\n",
			            pak.mbc().c_str(), pak.rambanks(), pak.rombanks());
			std::printf("header checksum: %s\n", pak.headerChecksumOk() ? "ok" : "bad");
			std::printf("cgb: %d\n", gambatte.isCgb());
		}

		sampleRate = rateOption.getRate();
		latency = latencyOption.getLatency();
		periods = periodsOption.getPeriods();

		resampler.reset(ResamplerInfo::get(resamplerOption.resamplerNumber()).create(2097152, sampleRate, inBuf.size() / 2));

		unsigned const gbbuts[8] = {
			InputGetter::START, InputGetter::SELECT,
			InputGetter::A, InputGetter::B,
			InputGetter::UP, InputGetter::DOWN,
			InputGetter::LEFT, InputGetter::RIGHT
		};

		for (unsigned i = 0; i < 8; ++i) {
			const InputOption::InputId &id = inputOption.getKeys()[i];

			if (id.type == InputOption::InputId::KEY) {
				keyMap.insert(std::pair<SDLKey,unsigned>(id.key, gbbuts[i]));
			} else {
				std::pair<JoyData,unsigned> pair(id.jdata, gbbuts[i]);
				jdevnums.push_back(id.jdata.dev_num);

				switch (id.type) {
				case InputOption::InputId::JOYBUT: jbMap.insert(pair); break;
				case InputOption::InputId::JOYAX: jaMap.insert(pair); break;
				case InputOption::InputId::JOYHAT: jhMap.insert(pair); break;
				default: break;
				}
			}
		}
	}

	gambatte.setInputGetter(&inputGetter);

	if (!jdevnums.empty()) {
		if (SDL_InitSubSystem(SDL_INIT_JOYSTICK) < 0) {
			std::fprintf(stderr, "Unable to init joysticks: %s\n", SDL_GetError());
			return EXIT_FAILURE;
		}
	}

	for (std::size_t i = 0; i < jdevnums.size(); ++i) {
		if (SDL_Joystick *const j = SDL_JoystickOpen(i))
			joysticks.push_back(j);
	}

	SDL_JoystickEventState(SDL_ENABLE);

	blitter.reset(new BlitterWrapper(VfilterInfo::get(vfOption.filterNumber()),
	                                 scaleOption.scale(), yuvOption.isSet(),
	                                 fsOption.isSet()));
	SDL_ShowCursor(SDL_DISABLE);
	SDL_WM_SetCaption("Gambatte SDL", 0);

	return 0;
}

bool GambatteSdl::handleEvents() {
	JoyData jd;
	SDL_Event e;

	while (SDL_PollEvent(&e)) switch (e.type) {
	case SDL_JOYAXISMOTION:
		jd.dev_num = e.jaxis.which;
		jd.num = e.jaxis.axis;
		jd.dir = e.jaxis.value < -8192
		       ? JoyData::DOWN
		       : (e.jaxis.value > 8192 ? JoyData::UP : JoyData::CENTERED);

		for (std::pair<jmap_t::iterator, jmap_t::iterator> range =
				jaMap.equal_range(jd); range.first != range.second; ++range.first) {
			if (jd.dir == range.first->first.dir)
				inputGetter.is |= range.first->second;
			else
				inputGetter.is &= ~range.first->second;
		}

		break;
	case SDL_JOYBUTTONDOWN:
	case SDL_JOYBUTTONUP:
		jd.dev_num = e.jbutton.which;
		jd.num = e.jbutton.button;

		for (std::pair<jmap_t::iterator, jmap_t::iterator> range =
				jbMap.equal_range(jd); range.first != range.second; ++range.first) {
			if (e.jbutton.state)
				inputGetter.is |= range.first->second;
			else
				inputGetter.is &= ~range.first->second;
		}

		break;
	case SDL_JOYHATMOTION:
		jd.dev_num = e.jhat.which;
		jd.num = e.jhat.hat;

		for (std::pair<jmap_t::iterator, jmap_t::iterator> range =
				jaMap.equal_range(jd); range.first != range.second; ++range.first) {
			if (e.jhat.value & range.first->first.dir)
				inputGetter.is |= range.first->second;
			else
				inputGetter.is &= ~range.first->second;
		}

		break;
	case SDL_KEYDOWN:
		if (e.key.keysym.mod & KMOD_CTRL) {
			switch (e.key.keysym.sym) {
			case SDLK_f: blitter->toggleFullScreen(); break;
			case SDLK_r: gambatte.reset(); break;
			default: break;
			}
		} else {
			switch (e.key.keysym.sym) {
			case SDLK_ESCAPE:
				return true;
			case SDLK_F5:
				gambatte.saveState(blitter->inBuf().pixels, blitter->inBuf().pitch);
				break;
			case SDLK_F6: gambatte.selectState(gambatte.currentState() - 1); break;
			case SDLK_F7: gambatte.selectState(gambatte.currentState() + 1); break;
			case SDLK_F8: gambatte.loadState(); break;
			case SDLK_0: gambatte.selectState(0); break;
			case SDLK_1: gambatte.selectState(1); break;
			case SDLK_2: gambatte.selectState(2); break;
			case SDLK_3: gambatte.selectState(3); break;
			case SDLK_4: gambatte.selectState(4); break;
			case SDLK_5: gambatte.selectState(5); break;
			case SDLK_6: gambatte.selectState(6); break;
			case SDLK_7: gambatte.selectState(7); break;
			case SDLK_8: gambatte.selectState(8); break;
			case SDLK_9: gambatte.selectState(9); break;
			default: break;
			}
		}
		// fallthrough
	case SDL_KEYUP:
		for (std::pair<keymap_t::iterator, keymap_t::iterator> range =
				keyMap.equal_range(e.key.keysym.sym);
				range.first != range.second; ++range.first) {
			if (e.key.state)
				inputGetter.is |= range.first->second;
			else
				inputGetter.is &= ~range.first->second;
		}

		break;
	case SDL_QUIT:
		return true;
	}

	return false;
}

int GambatteSdl::exec() {
	if (!gambatte.isLoaded())
		return 0;

	AudioData adata(sampleRate, latency, periods);
	tmpBuf.reset(resampler->maxOut(inBuf.size() / 2) * 2);

	Uint8 const *const keys = SDL_GetKeyState(0);
	unsigned samples = 0;
	bool audioBufLow = false;

	SDL_PauseAudio(0);

	for (;;) {
		if (bool done = handleEvents())
			return 0;

		const BlitterWrapper::Buf &vbuf = blitter->inBuf();
		unsigned emusamples = 35112 - samples;
		const int ret = gambatte.runFor(vbuf.pixels, vbuf.pitch,
				reinterpret_cast<gambatte::uint_least32_t*>(inBuf.get()) + samples, emusamples);
		const unsigned insamples = ret < 0 ? samples + emusamples : samples + ret;
		samples += emusamples;
		samples -= insamples;

		if (!keys[SDLK_TAB]) {
			const bool blit = ret >= 0 && !skipSched.skipNext(audioBufLow);

			if (blit)
				blitter->draw();

			const long outsamples = resampler->resample(tmpBuf, inBuf, insamples);
			const AudioData::Status &status = adata.write(tmpBuf, outsamples);
			audioBufLow = status.fromUnderrun + outsamples < (status.fromOverflow - outsamples) * 2;

			if (blit) {
				syncfunc((16743ul - 16743 / 1024) * sampleRate / status.rate);
				blitter->present();
			}
		} else if (ret >= 0) {
			blitter->draw();
			blitter->present();
		}

		std::memmove(inBuf, inBuf + insamples * 2, samples * 2 * sizeof *inBuf);
	}

	return 0;
}

} // anon namespace

int main(int argc, char **argv) {
	GambatteSdl gambatteSdl;
	if (int fail = gambatteSdl.init(argc, argv))
		return fail;

	return gambatteSdl.exec();
}
