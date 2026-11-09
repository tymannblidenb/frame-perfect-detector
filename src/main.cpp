#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <cmath>
#include <algorithm>
#include <filesystem>

using namespace geode::prelude;

// ---------------------------------------------------------------------------
// Идея точности:
//  * Считаем ФИЗИЧЕСКИЕ ШАГИ (processCommands вызывается один раз на шаг),
//    а не время и не FPS монитора. Один вход = точный номер шага.
//  * «Кадр» из настроек переводится в шаги: window = round(TPS / FPS).
//  * Frame perfect = интервал между двумя подряд идущими входами игрока
//    (нажатие->отпускание или отпускание->нажатие) не больше window шагов.
//  * Каждый интервал считается ровно один раз (нет двойного счёта).
// ---------------------------------------------------------------------------

namespace fp {

struct PlayerInput {
	int64_t lastPress = -1;
	int64_t lastRelease = -1;
};

struct State {
	int64_t step = 0;
	float stepDt = 1.f / 240.f;
	PlayerInput pl[2];
	int count = 0;
	int lastGap = -1;
	Ref<CCLabelBMFont> label;

	void resetAttempt(bool clearCount) {
		pl[0] = PlayerInput{};
		pl[1] = PlayerInput{};
		if (clearCount) {
			count = 0;
			lastGap = -1;
		}
	}
};

// Намеренно не удаляется: избегаем падений при выгрузке мода.
static State& S() {
	static State* s = new State();
	return *s;
}

static float currentTps() {
	auto manual = Mod::get()->getSettingValue<int64_t>("tps-override");
	if (manual > 0) return static_cast<float>(manual);
	auto& s = S();
	return s.stepDt > 0.f ? 1.f / s.stepDt : 240.f;
}

static int windowSteps() {
	auto fps = std::max<int64_t>(1, Mod::get()->getSettingValue<int64_t>("fps-threshold"));
	return std::max(1, static_cast<int>(std::lround(currentTps() / static_cast<float>(fps))));
}

static void refreshLabel(bool justHit) {
	auto& s = S();
	if (!s.label) return;

	auto mode = Mod::get()->getSettingValue<std::string>("display-mode");
	s.label->stopAllActions();

	if (mode == "Counter") {
		s.label->setString(fmt::format("FP: {}", s.count).c_str());
		s.label->setOpacity(200);
	}
	else if (justHit) {
		auto fps = Mod::get()->getSettingValue<int64_t>("fps-threshold");
		s.label->setString(fmt::format("{} FPS PERFECT ({} step)", fps, s.lastGap).c_str());
		s.label->setOpacity(255);
		float dur = Mod::get()->getSettingValue<int64_t>("window-duration") / 1000.f;
		s.label->runAction(CCSequence::create(CCDelayTime::create(dur), CCFadeOut::create(0.3f), nullptr));
	}
}

static void onFramePerfect(int gap) {
	auto& s = S();
	s.count++;
	s.lastGap = gap;
	refreshLabel(true);

	if (Mod::get()->getSettingValue<bool>("sound")) {
		float vol = Mod::get()->getSettingValue<int64_t>("sound-volume") / 100.f;
		auto path = Mod::get()->getResourcesDir() / "ding.wav";
		std::error_code ec;
		if (std::filesystem::exists(path, ec)) {
			FMODAudioEngine::sharedEngine()->playEffect(path.string(), 1.f, 0.f, vol);
		}
		else {
			// запасной звук игры, если ding.wav не нашёлся
			FMODAudioEngine::sharedEngine()->playEffect("counter003.ogg", 1.f, 0.f, vol);
		}
	}
}

} // namespace fp

class $modify(FPGameLayer, GJBaseGameLayer) {
	// Вызывается один раз на физический шаг: даёт точный счётчик шагов.
	void processCommands(float dt) {
		auto& s = fp::S();
		s.step++;
		float tw = m_gameState.m_timeWarp > 0.f ? m_gameState.m_timeWarp : 1.f;
		if (dt > 0.f) s.stepDt = dt / tw;
		GJBaseGameLayer::processCommands(dt);
	}

	// Кнопки обрабатываются внутри шага, значит step == шаг, на котором вход применился.
	void handleButton(bool down, int button, bool isPlayer1) {
		GJBaseGameLayer::handleButton(down, button, isPlayer1);

		if (button != 1) return; // только прыжок
		auto pl = PlayLayer::get();
		if (!pl || static_cast<GJBaseGameLayer*>(pl) != this) return;
		if (pl->m_isPracticeMode && !Mod::get()->getSettingValue<bool>("detect-in-practice")) return;

		auto& s = fp::S();
		auto& p = s.pl[isPlayer1 ? 0 : 1];
		int window = fp::windowSteps();

		if (down) {
			if (p.lastRelease >= 0) {
				int gap = static_cast<int>(s.step - p.lastRelease);
				if (gap <= window) fp::onFramePerfect(gap);
			}
			p.lastPress = s.step;
		}
		else {
			if (p.lastPress >= 0) {
				int gap = static_cast<int>(s.step - p.lastPress);
				if (gap <= window) fp::onFramePerfect(gap);
			}
			p.lastRelease = s.step;
		}
	}
};

class $modify(FPPlayLayer, PlayLayer) {
	void setupHasCompleted() {
		PlayLayer::setupHasCompleted();

		auto& s = fp::S();
		s.resetAttempt(true);

		auto win = CCDirector::get()->getWinSize();
		auto label = CCLabelBMFont::create("FP: 0", "bigFont.fnt");
		label->setScale(0.45f);
		label->setAnchorPoint({0.f, 1.f});
		label->setPosition({8.f, win.height - 8.f});
		label->setOpacity(200);
		label->setID("fp-label"_spr);
		m_uiLayer->addChild(label, 100);
		s.label = label;

		if (Mod::get()->getSettingValue<std::string>("display-mode") == "Window") {
			label->setOpacity(0);
		}
	}

	void resetLevel() {
		PlayLayer::resetLevel();
		bool clear = Mod::get()->getSettingValue<bool>("reset-on-restart");
		fp::S().resetAttempt(clear);
		fp::refreshLabel(false);
	}

	void onQuit() {
		fp::S().label = nullptr;
		PlayLayer::onQuit();
	}
};
