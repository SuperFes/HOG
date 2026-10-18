#include <hog/bot/bot.hpp>
#include <hog/core/log.hpp>

#include <glm/geometric.hpp>

#include <utility>

namespace hog::bot {

namespace {

const log::Category bot_log{"bot"};

} // namespace

Result<Bot> Bot::start(const planet::World& world, std::string address,
                       std::vector<std::byte> token, DVec3 direction, std::uint64_t seed,
                       const WanderSettings& settings) {
    auto player = client::Player::create(world, direction);
    if (!player) {
        return std::unexpected(player.error());
    }
    auto session = client::Session::connect(std::move(address), std::move(token));
    if (!session) {
        return std::unexpected(session.error());
    }
    return Bot{std::move(*player), std::move(*session), world.geology().planet_radius(), seed,
               settings};
}

Bot::Bot(client::Player player, client::Session session, double planet_radius, std::uint64_t seed,
         const WanderSettings& settings) noexcept
    : player_(std::move(player)), session_(std::move(session)), random_(seed),
      planet_radius_(planet_radius), settings_(settings) {}

void Bot::update(MonotonicClock::time_point now, double seconds) {
    session_.look(player_.feet(), now);
    if (const auto back = session_.take_correction()) {
        player_.place(*back);
    }
    Walked walked = walk(wander_, player_.feet(), planet_radius_, seconds, settings_);
    if (!walked.heading) {
        wander_ = pick(glm::normalize(player_.feet().metres()), planet_radius_, random_, settings_);
        return;
    }
    wander_ = walked.wander;
    const client::Flight aim{.eye = player_.feet(), .heading = *walked.heading, .pitch = 0.0};
    if (auto played = player_.play(client::stride(aim, DVec3{0.0, 0.0, 1.0}, false), seconds);
        !played) {
        log::warn(bot_log, "can't keep the ground: {}", played.error());
    }
}

} // namespace hog::bot
