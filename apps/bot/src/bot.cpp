#include <hog/bot/bot.hpp>
#include <hog/core/log.hpp>
#include <hog/globe/cell.hpp>

#include <glm/geometric.hpp>

#include <utility>

namespace hog::bot {

namespace {

const log::Category bot_log{"bot"};

} // namespace

Result<Bot> Bot::start(const globe::Generator& generator, std::string address,
                       std::vector<std::byte> token, DVec3 direction, std::uint64_t seed,
                       const WanderSettings& settings) {
    auto session = client::Session::connect(std::move(address), std::move(token));
    if (!session) {
        return std::unexpected(session.error());
    }
    return Bot{std::make_unique<Held>(generator), std::move(*session), glm::normalize(direction),
               seed, settings};
}

Bot::Bot(std::unique_ptr<Held> held, client::Session session, DVec3 direction, std::uint64_t seed,
         const WanderSettings& settings) noexcept
    : held_(std::move(held)), direction_(direction), session_(std::move(session)), random_(seed),
      planet_radius_(held_->world.generator().planet().radius), settings_(settings) {}

WorldPosition Bot::feet() const {
    return player_ ? player_->feet() : WorldPosition{direction_ * planet_radius_};
}

void Bot::take_ground() {
    for (const protocol::SentColumn& column : session_.take_columns()) {
        if (auto taken =
                held_->cache.take(column.face, column.i, column.j, column.level, column.bytes);
            !taken) {
            log::warn(bot_log, "can't hold a tile column sent: {}", taken.error());
        }
    }
    const globe::Layers& layers = held_->world.layers();
    const globe::Cell cell = globe::cell_under(glm::normalize(feet().metres()), layers.level);
    constexpr auto side = static_cast<std::uint32_t>(globe::Volume::tile_side);
    const globe::Site over{.face = cell.face, .i = cell.i / side * side, .j = cell.j / side * side};
    if (held_->over != over) {
        held_->over = over;
        held_->cache.keep_near(layers, feet().metres(), keeps);
    }
    const std::vector<globe::Site> changed = held_->cache.take_changed();
    if (player_) {
        for (const globe::Site corner : changed) {
            for (const globe::BrickSite brick : globe::brick_footprint(corner)) {
                player_->changed(brick);
            }
        }
    } else if (held_->volume.floor(over)) {
        auto player = client::Player::create(held_->world, direction_);
        if (!player) {
            log::warn(bot_log, "can't stand on the ground sent: {}", player.error());
            return;
        }
        player_ = std::move(*player);
    }
}

void Bot::update(MonotonicClock::time_point now, double seconds) {
    // Until it stands it hangs where it's to, flying, which the server takes as it is.
    session_.look(client::Whereabouts{.at = feet(),
                                      .stance = player_ ? protocol::Stance::Walking
                                                        : protocol::Stance::Flying},
                  now);
    take_ground();
    if (!player_) {
        return;
    }
    if (const auto back = session_.take_correction()) {
        player_->place(*back);
    }
    Walked walked = walk(wander_, player_->feet(), planet_radius_, seconds, settings_);
    if (!walked.heading) {
        wander_ =
            pick(glm::normalize(player_->feet().metres()), planet_radius_, random_, settings_);
        return;
    }
    wander_ = walked.wander;
    const client::Flight aim{.eye = player_->feet(), .heading = *walked.heading, .pitch = 0.0};
    if (auto played = player_->play(client::stride(aim, DVec3{0.0, 0.0, 1.0}, false), seconds);
        !played) {
        log::warn(bot_log, "can't keep the ground: {}", played.error());
    }
}

} // namespace hog::bot
