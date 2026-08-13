#include "havoc/eval/hce.hpp"

#include "havoc/bitboard.hpp"
#include "havoc/magics.hpp"
#include "havoc/kpk.hpp"
#include "havoc/position.hpp"
#include "havoc/squares.hpp"
#include "havoc/utils.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace havoc {

// ─── Mobility tables ────────────────────────────────────────────────────────

namespace {

inline bool is_pawnless_endgame(const position& p) {
    return (p.get_pieces<white, pawn>() | p.get_pieces<black, pawn>()) == 0ULL;
}

/// True when `c` holds the classic drawn "wrong rook pawn" ending: every pawn
/// on a single rook file, at least one bishop, no bishop able to touch the
/// promotion square, and the defending king already controlling that square.
///
/// This is a theorem rather than a heuristic. The pawn can only promote on the
/// corner square; the defending king sits on or beside it and can never be
/// driven away, because the attacking bishop covers the opposite color complex
/// and so can neither take the corner nor cover the escape square. The attacker
/// may have any number of pawns and any number of bishops -- if they are all on
/// the rook file and all on the wrong color, none of that matters.
///
/// haVoc had no notion of this at all. `k7/8/8/8/8/8/PB6/6K1 w` (a-pawn, dark
/// bishop, black king on a8) is a dead draw and scored +443, one point off the
/// +441 it gave the genuinely winning light-bishop version of the same
/// position. The engine would happily trade into it believing it was a pawn up.
template <Color c> inline bool is_wrong_rook_pawn_draw(const position& p) {
    constexpr Color them = (c == white ? black : white);

    // The defender must be down to a bare king. Any other material gives
    // counterplay, or a piece to sacrifice for the pawn, and the proof breaks.
    if ((p.get_pieces<them, pawn>() | p.get_pieces<them, knight>() |
         p.get_pieces<them, bishop>() | p.get_pieces<them, rook>() |
         p.get_pieces<them, queen>()) != 0ULL)
        return false;

    // The attacker must hold nothing but king, pawns and bishops. A knight or
    // a rook covers the corner the bishop cannot, and wins.
    if ((p.get_pieces<c, knight>() | p.get_pieces<c, rook>() | p.get_pieces<c, queen>()) != 0ULL)
        return false;

    const U64 pawns = p.get_pieces<c, pawn>();
    const U64 bishops = p.get_pieces<c, bishop>();
    if (pawns == 0ULL || bishops == 0ULL)
        return false;

    // Every pawn on the a-file, or every pawn on the h-file.
    int promo_col;
    if ((pawns & ~bitboards::col[Col::A]) == 0ULL)
        promo_col = Col::A;
    else if ((pawns & ~bitboards::col[Col::H]) == 0ULL)
        promo_col = Col::H;
    else
        return false;

    const int promo = (c == white ? 56 + promo_col : promo_col);

    // Derive the promotion square's color from the board rather than assuming a
    // convention, then require every bishop to be on the other complex.
    const U64 promo_complex = (bitboards::squares[promo] & bitboards::colored_sqs[white]) != 0ULL
                                  ? bitboards::colored_sqs[white]
                                  : bitboards::colored_sqs[black];
    if ((bishops & promo_complex) != 0ULL)
        return false;

    // The defending king must already be on or beside the promotion square.
    const int dk = static_cast<int>(p.king_square(them));
    return std::max(util::row_dist(dk, promo), util::col_dist(dk, promo)) <= 1;
}

/// The two long diagonals, a1-h8 and a8-h1. Squares are indexed row*8 + col,
/// so a1-h8 is every ninth square from a1 and a8-h1 every seventh from h1.
constexpr U64 kLongDiagonals = [] {
    U64 m = 0ULL;
    for (int i = 0; i < 8; ++i) {
        m |= 1ULL << (i * 9);
        m |= 1ULL << (7 + i * 7);
    }
    return m;
}();

/// Evaluation for a pawnless ending in which one side is down to a lone king.
///
/// The ordinary evaluation is completely flat here. Every strong-side move
/// leaves the same material, the same (empty) pawn structure and the same
/// undefended enemy king, so the search looks out over a plateau, finds nothing
/// to climb, and shuffles until the repetition or fifty-move rule rescues the
/// defender. That is not a subtle weakness: haVoc could not win king and two
/// bishops against a bare king, nor king, bishop and knight, from a standard
/// starting position at any depth. Giving the score a gradient -- drive the
/// defending king to the edge, and walk the attacking king in behind it --
/// turns the plateau into a hill, which is all the search needs to find mate.
inline int eval_bare_king(const position& p, Color strong, const parameters& params) {
    const int sk = static_cast<int>(p.king_square(strong));
    const int wk = static_cast<int>(p.king_square(strong == white ? black : white));

    // Read the tuned piece values rather than a private table, so this path
    // stays on the same scale as the rest of the evaluation and the tuner keeps
    // its grip on material_value in these positions.
    int score = 0;
    for (int pc = pawn; pc < king; ++pc)
        score += params.material_value[pc] * static_cast<int>(p.number_of(strong, Piece(pc)));

    // 90 in a corner, falling to -90 in the centre. The distance is squared so
    // that the last step out to the edge is worth much more than the first,
    // which is what stops the defending king drifting back to safety.
    const int cf = std::min(sq_col(wk), 7 - sq_col(wk));
    const int cr = std::min(sq_row(wk), 7 - sq_row(wk));
    score += 90 - 10 * (cf * cf + cr * cr);

    // Mate needs the attacking king in opposition, and the defender has nothing
    // better to do than run, so closing the gap is always progress.
    score += 70 - 10 * std::max(row_dist(sk, wk), col_dist(sk, wk));

    // Bishop and knight mate only in the two corners the bishop can reach, so
    // aim at those specifically -- the generic edge term above is happy to herd
    // the king into a corner where no mate exists, and the fifty-move rule then
    // runs out while the engine tries to start again.
    if (p.number_of(strong, bishop) == 1 && p.number_of(strong, knight) == 1 &&
        p.number_of(strong, rook) == 0 && p.number_of(strong, queen) == 0) {
        const U64 bishops =
            strong == white ? p.get_pieces<white, bishop>() : p.get_pieces<black, bishop>();
        const int bs = bits::lsb(bishops);
        const bool dark = ((sq_col(bs) + sq_row(bs)) & 1) == 0;
        const int c1 = dark ? A1 : H1;
        const int c2 = dark ? H8 : A8;
        const int d1 = std::max(row_dist(wk, c1), col_dist(wk, c1));
        const int d2 = std::max(row_dist(wk, c2), col_dist(wk, c2));
        score += 100 - 20 * std::min(d1, d2);
    }

    return score;
}

} // namespace

// ─── Constructor ────────────────────────────────────────────────────────────

HCEEvaluator::HCEEvaluator(pawn_table& pt, material_table& mt, const parameters& params)
    : pawn_table_(pt), material_table_(mt), params_(params) {}

// ─── Main evaluate ──────────────────────────────────────────────────────────

// Every exit from evaluate() goes through this. The running `score` is
// white-relative; the caller wants a side-to-move-relative score. The tempo
// bonus is a property of *having the move*, so it is added after the flip --
// adding it before would hand the bonus to white and a penalty to black.
static inline int to_stm(const position& p, int score, int tempo) {
    return (p.to_move() == white ? score : -score) + tempo;
}

int HCEEvaluator::evaluate(const position& p, int lazy_margin) {
    // A dead position is worth exactly nothing to either side. Without this the
    // material term happily reports a bishop or knight up in a king-and-minor
    // ending, which is how the search traded into one: the stand pat in qsearch,
    // reverse futility and null move pruning all read this number.
    if (p.is_material_draw())
        return score::kDraw;

    // A lone king with no pawns left on the board needs the dedicated gradient
    // above, and it has to be answered here rather than at the end of the
    // function: the material imbalance is enormous, so both lazy-evaluation
    // exits fire long before the positional terms are ever reached.
    if (is_pawnless_endgame(p)) {
        if (p.get_pieces<black>() == p.get_pieces<black, king>())
            return to_stm(p, eval_bare_king(p, white, params_), 0);
        if (p.get_pieces<white>() == p.get_pieces<white, king>())
            return to_stm(p, -eval_bare_king(p, black, params_), 0);
    }

    int score = 0;
    einfo ei{};

    ei.pe = pawn_table_.fetch(p);
    ei.me = material_table_.fetch(p);

    ei.all_pieces = p.all_pieces();
    ei.empty = ~p.all_pieces();
    ei.pieces[white] = p.get_pieces<white>();
    ei.pieces[black] = p.get_pieces<black>();
    ei.weak_pawns[white] = ei.pe->doubled[white] | ei.pe->isolated[white] | ei.pe->backward[white] |
                           ei.pe->undefended[white];
    ei.weak_pawns[black] = ei.pe->doubled[black] | ei.pe->isolated[black] | ei.pe->backward[black] |
                           ei.pe->undefended[black];
    ei.kmask[white] = bitboards::kmask[p.king_square(white)];
    ei.kmask[black] = bitboards::kmask[p.king_square(black)];
    // colored_sqs[white] is the light squares and colored_sqs[black] the dark
    // ones; see the initialiser in bitboard.cpp. These two lines are what makes
    // the bad-bishop penalty in eval_bishops do anything at all -- the masks it
    // reads were declared and read but never written, so they were always zero.
    ei.light_sq_pawns[white] = p.get_pieces<white, pawn>() & bitboards::colored_sqs[white];
    ei.dark_sq_pawns[white] = p.get_pieces<white, pawn>() & bitboards::colored_sqs[black];
    ei.light_sq_pawns[black] = p.get_pieces<black, pawn>() & bitboards::colored_sqs[white];
    ei.dark_sq_pawns[black] = p.get_pieces<black, pawn>() & bitboards::colored_sqs[black];
    ei.queen_sqs[white] = p.get_pieces<white, queen>();
    ei.queen_sqs[black] = p.get_pieces<black, queen>();
    ei.pawn_holes[white] = ei.pe->backward[white] << 8;
    ei.pawn_holes[black] = ei.pe->backward[black] >> 8;

    // Pawn material is a material term and goes in unscaled. The structural
    // terms are tapered (the pawn hash stores both ends, being keyed on
    // structure alone) and then scaled by the pawn-structure category, which
    // until now only reached a one-point king-harassment term in eval_pawns
    // while the real pawn structure bypassed it entirely.
    score += ei.pe->material;
    score += ((ei.pe->score_eg * ei.me->phase_interpolant +
               ei.pe->score_mg * (24 - ei.me->phase_interpolant)) /
              24) *
             params_.pawn_structure_category_scale / 100;
    score += ei.me->score;

    // Lazy eval cutoff
    if (lazy_margin > 0 && !ei.me->is_endgame() && std::abs(score) >= lazy_margin)
        return to_stm(p, score, params_.tempo);

    // Endgame specialization
    if (ei.me->is_endgame()) {
        EndgameType egt = ei.me->endgame;
        if (is_pawnless_endgame(p)) {
            if (egt == KpK || egt == KnK || egt == KbK || egt == KnnK || egt == KbbK ||
                egt == KbnK || egt == KnbK) {
                return score::kDraw;
            }
        }
        switch (egt) {
        case KpK: {
            // A single pawn against a bare king is solved exactly by the
            // bitbase, so there is nothing here for the evaluation to guess at.
            const U64 wp = p.get_pieces<white, pawn>();
            const U64 bp = p.get_pieces<black, pawn>();
            if (bits::count(wp | bp) == 1) {
                const Color strong = wp ? white : black;
                U64 pawns = wp | bp;
                int psq = bits::lsb(pawns);
                int sk = static_cast<int>(p.king_square(strong));
                int wk_ = static_cast<int>(p.king_square(strong == white ? black : white));
                // Normalise so the pawn is white's and stands on files a-d.
                if (strong == black) {
                    psq ^= 56;
                    sk ^= 56;
                    wk_ ^= 56;
                }
                if (util::col(psq) > Col::D) {
                    psq ^= 7;
                    sk ^= 7;
                    wk_ ^= 7;
                }
                const Color stm = (p.to_move() == strong) ? white : black;
                if (!kpk::probe(sk, psq, wk_, stm))
                    return score::kDraw;
            }
            score += eval_kpk<white>(p, ei) - eval_kpk<black>(p, ei);
            break;
        }
        case KrrK:
            score += eval_krrk<white>(p, ei) - eval_krrk<black>(p, ei);
            break;
        case KRK:
            score += eval_krk<white>(p, ei) - eval_krk<black>(p, ei);
            break;
        case KQK:
            score += eval_kqk<white>(p, ei) - eval_kqk<black>(p, ei);
            break;
        case KBNK:
            score += eval_kbnk<white>(p, ei) - eval_kbnk<black>(p, ei);
            break;
        default:
            break;
        }
    }

    // Apply category-level scale factors (percentage: 100 = 1.0x)
    int pawn_score = eval_pawns<white>(p, ei) - eval_pawns<black>(p, ei);
    int piece_score = (eval_knights<white>(p, ei) - eval_knights<black>(p, ei)) +
                      (eval_bishops<white>(p, ei) - eval_bishops<black>(p, ei)) +
                      (eval_rooks<white>(p, ei) - eval_rooks<black>(p, ei)) +
                      (eval_queens<white>(p, ei) - eval_queens<black>(p, ei));
    int king_score = eval_king<white>(p, ei) - eval_king<black>(p, ei);
    int passed_score = eval_passed_pawns<white>(p, ei) - eval_passed_pawns<black>(p, ei);

    score += (pawn_score * params_.pawn_structure_category_scale) / 100;
    score += (piece_score * params_.sq_score_category_scale) / 100;
    score += (king_score * params_.king_safety_category_scale) / 100;
    score += (passed_score * ei.me->taper(params_.passed_pawn_category_scale,
                                          params_.passed_pawn_endgame_scale)) /
             100;

    if (lazy_margin > 0 && !ei.me->is_endgame() && std::abs(score) >= lazy_margin)
        return to_stm(p, score, params_.tempo);

    int threat_score = eval_threats<white>(p, ei) - eval_threats<black>(p, ei);
    int space_score = eval_space<white>(p, ei) - eval_space<black>(p, ei);

    score += (threat_score * params_.threat_category_scale) / 100;
    score += (space_score * params_.space_category_scale) / 100;

    // Endgame scaling
    int scale = 128;

    // Opposite-color bishops → drawish, but only in a *pure* opposite-colored
    // bishop ending: one bishop each, on opposite colors, and no other pieces.
    //
    // The old test looked at bishop colors alone. Any position in which each
    // side happened to hold a single bishop on opposite colors -- an extremely
    // common middlegame shape -- had its entire evaluation multiplied by
    // opposite_bishop_scale/128 = 24/128 = 0.19. On a middlegame with both
    // queens, all four rooks and all four knights still on, white a clean pawn
    // up, that reported +0.26 instead of +1.42.
    //
    // The drawishness of opposite bishops comes from the defender's bishop
    // guarding a color complex the attacker can never contest, which only holds
    // once the bishops are the last pieces. With a rook or queen still on, the
    // extra piece covers the missing color and opposite bishops make the
    // position *sharper*, since the attacker effectively plays a piece up on
    // the squares the defending bishop cannot see.
    const U64 white_bishops = p.get_pieces<white, bishop>();
    const U64 black_bishops = p.get_pieces<black, bishop>();
    const bool bishops_only =
        (p.get_pieces<white, knight>() | p.get_pieces<black, knight>() |
         p.get_pieces<white, rook>() | p.get_pieces<black, rook>() | p.get_pieces<white, queen>() |
         p.get_pieces<black, queen>()) == 0ULL;
    const bool one_bishop_each = bits::count(white_bishops) == 1 && bits::count(black_bishops) == 1;
    const bool opposite_colors = ((white_bishops & bitboards::colored_sqs[white]) != 0ULL) !=
                                 ((black_bishops & bitboards::colored_sqs[white]) != 0ULL);
    if (bishops_only && one_bishop_each && opposite_colors)
        scale = std::min(scale, params_.opposite_bishop_scale);

    // No pawns with small material advantage → likely drawn
    if (is_pawnless_endgame(p) && std::abs(score) < 400)
        scale = std::min(scale, params_.no_pawn_scale);
    // Single minor piece advantage with no pawns → near draw
    if (is_pawnless_endgame(p) && std::abs(ei.me->score) <= 315 && std::abs(ei.me->score) > 0)
        scale = std::min(scale, params_.minor_advantage_no_pawn_scale);

    // Wrong rook pawn plus wrong-colored bishop: a proven draw, so the whole
    // score collapses regardless of how many pawns the attacker is up.
    if (is_wrong_rook_pawn_draw<white>(p) || is_wrong_rook_pawn_draw<black>(p))
        scale = std::min(scale, params_.wrong_rook_pawn_scale);

    if (scale != 128)
        score = (score * scale) / 128;

    return to_stm(p, score, params_.tempo);
}

// ─── eval_pawns ─────────────────────────────────────────────────────────────

template <Color c> int HCEEvaluator::eval_pawns(const position& p, einfo& ei) {
    int score = 0;
    constexpr Color them = Color(c ^ 1);
    U64 pawnAttacks = ei.pe->attacks[c];

    // Pawn harassment of enemy king
    U64 kattks = pawnAttacks & ei.kmask[them];
    int kAttkCount = bits::count(kattks);
    if (kAttkCount) {
        ei.kattackers[c][pawn]++;
        ei.kattk_points[c][pawn] |= kattks;
        score += params_.pawn_king[std::min(2, kAttkCount)];
    }

    // Pawn chain bases / undefended enemy pawns
    U64 baseAttks = pawnAttacks & ei.pe->undefended[them];
    score += bits::count(baseAttks) / 2;
    return score;
}

// ─── eval_knights ───────────────────────────────────────────────────────────

template <Color c> int HCEEvaluator::eval_knights(const position& p, einfo& ei) {
    int score = 0;
    constexpr Color them = Color(c ^ 1);
    Square* knights = p.squares_of<c, knight>();
    U64 equeen_sq = ei.queen_sqs[them];
    int ks = p.king_square(c);

    for (Square s = *knights; s != no_square; s = *++knights) {
        U64 sq_bb = bitboards::squares[s];

        score +=
            params_.sq_score_scaling[knight] * square_score<c>(params_, knight, s, ei.me->phase_interpolant);

        // Mobility
        U64 mvs = bitboards::nmask[s];
        ei.piece_attacks[c][knight] |= mvs;
        if (!(sq_bb & p.pinned<c>())) {
            U64 mobility = (mvs & ei.empty) & (~ei.pe->attacks[them]);
            unsigned cnt = bits::count(mobility);
            int mob = (cnt < params_.knight_mobility_table.size())
                          ? params_.knight_mobility_table[cnt]
                          : params_.knight_mobility_table.back();
            score += ((params_.knight_mobility_scale * params_.mobility_scaling[knight] * mob) /
                      100) *
                     ei.me->taper(params_.mobility_category_scale, params_.mobility_endgame_scale) / 100;
        }

        // Outpost
        if (sq_bb & ei.pawn_holes[them])
            score += params_.knight_outpost_bonus[util::col(s)];

        // Edge penalty
        if (sq_bb & bitboards::edges)
            score -= 12;

        // Closed center bonus
        if (ei.pe->locked_center || ei.pe->center_pawn_count >= 4)
            score += params_.bishop_open_center_bonus;

        // Center influence
        U64 center_influence = mvs & bitboards::big_center_mask;
        score += bits::count(center_influence) * params_.center_influence_bonus[knight];

        // Queen attack
        U64 qattks = mvs & equeen_sq;
        score += bits::count(qattks) * params_.attk_queen_bonus[knight];

        // King distance
        int dist = std::max(util::row_dist(s, ks), util::col_dist(s, ks));
        score -= dist;

        // Minor behind pawn
        auto fsq = (c == white ? s + 8 : s - 8);
        if (util::on_board(fsq)) {
            auto bbs = bitboards::squares[fsq];
            auto pawninfront = p.get_pieces<c, pawn>() & bbs;
            if (pawninfront && util::row(s) != Row::r1 && util::row(s) != Row::r8)
                score += 12;
        }

        // King harassment
        U64 kattks = mvs & ei.kmask[them];
        if (kattks) {
            ei.kattackers[c][knight]++;
            ei.kattk_points[c][knight] |= kattks;
            score += params_.knight_king[std::min(2, bits::count(kattks))];
        }

        // Protection
        score += bits::count(p.attackers_of2(s, c));
    }
    return score;
}

// ─── eval_bishops ───────────────────────────────────────────────────────────

template <Color c> int HCEEvaluator::eval_bishops(const position& p, einfo& ei) {
    int score = 0;
    constexpr Color them = Color(c ^ 1);
    Square* bishops = p.squares_of<c, bishop>();
    bool dark_sq = false;
    bool light_sq = false;
    U64 flight_sq_pawns = ei.light_sq_pawns[c];
    U64 fdark_sq_pawns = ei.dark_sq_pawns[c];
    U64 equeen_sq = ei.queen_sqs[them];
    U64 valuable_enemies =
        p.get_pieces<them, queen>() | p.get_pieces<them, rook>() | p.get_pieces<them, king>();
    int ks = p.king_square(c);

    for (Square s = *bishops; s != no_square; s = *++bishops) {
        U64 sq_bb = bitboards::squares[s];

        score +=
            params_.sq_score_scaling[bishop] * square_score<c>(params_, bishop, s, ei.me->phase_interpolant);

        // The colour of the square this bishop is on. light_sq / dark_sq below
        // are running "have we seen one of these" flags for the bishop pair
        // test at the end of the function, so they must not be used to ask
        // about the bishop currently in hand.
        const bool this_light = (sq_bb & bitboards::colored_sqs[white]) != 0ULL;

        // Which square colors this side has a bishop on, for the pair bonus at
        // the end of the function.
        if (this_light)
            light_sq = true;
        else
            dark_sq = true;

        // X-Ray attacks on valuable pieces
        score += bits::count(bitboards::battks[s] & valuable_enemies);

        // Mobility
        U64 mvs = magics::attacks<bishop>(ei.all_pieces, s);
        ei.piece_attacks[c][bishop] |= mvs;
        U64 mobility = (mvs & ei.empty) & (~ei.pe->attacks[them]);

        unsigned mob_cnt = bits::count(mobility);
        int mob_val = (mob_cnt < params_.bishop_mobility_table.size())
                          ? params_.bishop_mobility_table[mob_cnt]
                          : params_.bishop_mobility_table.back();
        int mobility_score =
            ((params_.bishop_mobility_scale * params_.mobility_scaling[bishop] * mob_val) / 100) *
            ei.me->taper(params_.mobility_category_scale, params_.mobility_endgame_scale) / 100;
        if ((sq_bb & p.pinned<c>()) && mobility_score > 0)
            mobility_score /= params_.pinned_scaling[bishop];

        score += mobility_score;

        // King distance
        score -= std::max(util::row_dist(s, ks), util::col_dist(s, ks));

        // Closed center penalty
        if (ei.pe->locked_center || ei.pe->center_pawn_count >= 4)
            score -= params_.bishop_open_center_bonus;

        // Center influence
        score +=
            bits::count(mvs & bitboards::big_center_mask) * params_.center_influence_bonus[bishop];

        // Long diagonal bonus. This has to be the two genuinely long diagonals,
        // a1-h8 and a8-h1, and nothing else. The old test asked whether the
        // bishop stood anywhere a bishop on d5 (for light squares) or e5 (for
        // dark squares) could see, which is the long diagonal *plus* a second,
        // shorter one -- a2-g8 for light, b8-h2 for dark. Those two extras are
        // not mirror images of each other, so the bonus was handed out on
        // different squares depending on color and the evaluation was not
        // symmetric. A bishop on c4 collected it while its mirror on c5 did
        // not; a bishop on g3 collected it while its mirror on g6 did not.
        //
        // The union of the two long diagonals is mirror invariant, and a
        // bishop can only ever stand on the one matching its square color, so
        // a single mask is both correct and symmetric.
        if (sq_bb & kLongDiagonals)
            score += params_.bishop_open_center_bonus;

        // Outpost
        if (sq_bb & ei.pawn_holes[them])
            score += params_.bishop_outpost_bonus[util::col(s)];

        // Minor behind pawn
        auto fsq = (c == white ? s + 8 : s - 8);
        if (util::on_board(fsq)) {
            auto bbs = bitboards::squares[fsq];
            auto pawninfront = p.get_pieces<c, pawn>() & bbs;
            if (pawninfront && util::row(s) != Row::r1 && util::row(s) != Row::r8)
                score += 12;
        }

        // Penalty for bishops on same color as own pawns.
        //
        // Blended by game phase. Selecting on is_endgame() picked the endgame
        // value only once the board was down to two non-pawn pieces, which is
        // 8.5% of positions, so bishop_own_pawn_penalty_eg was very nearly a
        // dead parameter and a bad bishop was charged the middlegame rate
        // through three quarters of all real endgames.
        int same_color_penalty =
            ei.me->taper(params_.bishop_own_pawn_penalty_mg, params_.bishop_own_pawn_penalty_eg);
        U64 fcolored_pawns = (this_light ? flight_sq_pawns : fdark_sq_pawns);
        score -= same_color_penalty * bits::count(fcolored_pawns);

        // Queen attacks
        score += bits::count(mvs & equeen_sq) * params_.attk_queen_bonus[bishop];

        // King harassment
        U64 kattks = mvs & ei.kmask[them];
        int king_attk_count = bits::count(kattks);
        if (king_attk_count) {
            ei.kattackers[c][bishop]++;
            ei.kattk_points[c][bishop] |= kattks;
            score += params_.bishop_king[std::min(2, king_attk_count)];
        }

        // Protection
        score += bits::count(p.attackers_of2(s, c));
    }

    // Double bishop bonus
    if (light_sq && dark_sq)
        score += params_.doubled_bishop_bonus;

    return score;
}

// ─── eval_rooks ─────────────────────────────────────────────────────────────

template <Color c> int HCEEvaluator::eval_rooks(const position& p, einfo& ei) {
    int score = 0;
    int rookIdx = 0;
    Square rookSquares[10] = {};
    std::fill(std::begin(rookSquares), std::end(rookSquares), no_square);

    Square* rooks = p.squares_of<c, rook>();
    constexpr Color them = Color(c ^ 1);
    U64 equeen_sq = ei.queen_sqs[them];
    U64 valuable_enemies = p.get_pieces<them, queen>() | p.get_pieces<them, king>();
    U64 all_pawns = p.get_pieces<white, pawn>() | p.get_pieces<black, pawn>();

    for (Square s = *rooks; s != no_square; s = *++rooks) {
        U64 sq_bb = bitboards::squares[s];

        score +=
            params_.sq_score_scaling[rook] * square_score<c>(params_, rook, s, ei.me->phase_interpolant);

        rookSquares[rookIdx++] = s;

        // X-Ray attacks on valuable pieces
        score += bits::count(bitboards::rattks[s] & valuable_enemies);

        // Mobility
        U64 mvs = magics::attacks<rook>(ei.all_pieces, s);
        ei.piece_attacks[c][rook] |= mvs;
        U64 mobility = (mvs & ei.empty) & (~ei.pe->attacks[them]);

        int free_sqs = bits::count(mobility);
        int mob_r = (static_cast<unsigned>(free_sqs) < params_.rook_mobility_table.size())
                        ? params_.rook_mobility_table[free_sqs]
                        : params_.rook_mobility_table.back();
        int mobility_score =
            ((params_.rook_mobility_scale * params_.mobility_scaling[rook] * mob_r) / 100) *
            ei.me->taper(params_.mobility_category_scale, params_.mobility_endgame_scale) / 100;

        if (sq_bb & p.pinned<c>())
            mobility_score /= params_.pinned_scaling[rook];

        score += mobility_score;

        // Trapped rook. Blended by phase for the same reason as the bishop
        // penalty above: index 1 of trapped_rook_penalty was reachable only in
        // the barest positions. The extra charge for having not yet castled is
        // a middlegame idea, so it fades with the middlegame weight instead of
        // switching off.
        if (trapped_rook<c>(p, ei, s)) {
            score -= ei.me->taper(params_.trapped_rook_penalty[0], params_.trapped_rook_penalty[1]);
        }

        // Center influence
        score +=
            bits::count(mvs & bitboards::big_center_mask) * params_.center_influence_bonus[rook];

        // Queen attacks
        score += bits::count(mvs & equeen_sq) * params_.attk_queen_bonus[rook];

        // Open file
        if ((bitboards::col[util::col(s)] & all_pawns) == 0ULL)
            score += params_.open_file_bonus;

        // 7th rank
        if (sq_bb & (c == white ? bitboards::row[r7] : bitboards::row[r2]))
            score += params_.rook_7th_bonus;

        // King harassment
        U64 kattks = mvs & ei.kmask[them];
        int king_attk_count = bits::count(kattks);
        if (king_attk_count) {
            ei.kattackers[c][rook]++;
            ei.kattk_points[c][rook] |= kattks;
            score += params_.rook_king[std::min(4, king_attk_count)];
        }

        // Protection
        score += bits::count(p.attackers_of2(s, c));
    }

    // Connected rook bonus
    if (rookIdx >= 2) {
        int row0 = util::row(rookSquares[0]);
        int row1 = util::row(rookSquares[1]);
        int col0 = util::col(rookSquares[0]);
        int col1 = util::col(rookSquares[1]);

        if ((row0 == row1) || (col0 == col1)) {
            U64 between_bb = bitboards::between[rookSquares[0]][rookSquares[1]];
            U64 sq_bb = bitboards::squares[rookSquares[0]] | bitboards::squares[rookSquares[1]];
            U64 blockers = (between_bb ^ sq_bb) & ei.all_pieces;

            if (blockers == 0ULL)
                score += params_.connected_rook_bonus;
        }
    }

    return score;
}

// ─── eval_queens ────────────────────────────────────────────────────────────

template <Color c> int HCEEvaluator::eval_queens(const position& p, einfo& ei) {
    int score = 0;
    constexpr Color them = Color(c ^ 1);
    Square* queens = p.squares_of<c, queen>();
    U64 weakEnemies = p.get_pieces<them, pawn>() | p.get_pieces<them, knight>() |
                      p.get_pieces<them, bishop>() | p.get_pieces<them, rook>();

    for (Square s = *queens; s != no_square; s = *++queens) {
        // Square eval
        score +=
            params_.sq_score_scaling[queen] * square_score<c>(params_, queen, s, ei.me->phase_interpolant);

        // Mobility
        U64 mvs =
            magics::attacks<bishop>(ei.all_pieces, s) | magics::attacks<rook>(ei.all_pieces, s);
        ei.piece_attacks[c][queen] |= mvs;

        // Weak queen penalty
        U64 attackers = p.attackers_of2(s, them) & weakEnemies;
        score -= bits::count(attackers);

        // Center influence
        score +=
            bits::count(mvs & bitboards::big_center_mask) * params_.center_influence_bonus[queen];

        // King harassment
        U64 kattks = mvs & ei.kmask[them];
        int king_attk_count = bits::count(kattks);
        if (king_attk_count) {
            ei.kattackers[c][queen]++;
            ei.kattk_points[c][queen] |= kattks;
            score += params_.queen_king[std::min(6, king_attk_count)];
        }
    }

    return score;
}

// ─── eval_king ──────────────────────────────────────────────────────────────

template <Color c> int HCEEvaluator::eval_king(const position& p, einfo& ei) {
    int score = 0;
    constexpr Color them = Color(c ^ 1);
    Square* kings = p.squares_of<c, king>();
    U64 enemyPawns = p.get_pieces<them, pawn>();
    // Shelter, storm and the castling bonus are middlegame ideas: they fade
    // out as the pieces that could exploit a draughty king leave the board.
    // They used to switch off in one step when the material happened to match
    // a classified ending, which is 8.5% of positions, so a king was still
    // charged full middlegame shelter through most real endgames and then had
    // the whole term vanish at an arbitrary boundary.
    const int mg = ei.me->mg_weight();

    for (Square s = *kings; s != no_square; s = *++kings) {
        U64 sq_bb = bitboards::squares[s];

        // Square eval. square_score already tapers between the middlegame and
        // endgame king tables by phase, so gating it as well left the king with
        // no positional guidance at all in exactly the positions where king
        // activity decides the game. Classified endings that supply their own
        // king logic -- KPK through the bitbase, KRK, KBNK -- do so by adding
        // to this score, and the endgame king table already wants the king
        // centralised, so there is nothing here to double count.
        score += params_.sq_score_scaling[king] *
                 square_score<c>(params_, king, s, ei.me->phase_interpolant);

        // Mobility
        U64 mvs = ei.kmask[c] & ei.empty;

        // King safety - attackers
        U64 unsafe_bb = 0ULL;
        for (Piece pc = pawn; pc <= queen; ++pc)
            unsafe_bb |= ei.kattk_points[them][pc];

        // Safe checks. The most concrete form of king danger is a square an
        // enemy piece already attacks, from which it would give check, and
        // which we do not defend -- the check simply lands and cannot be
        // answered by taking the checker.
        //
        // The check-from sets are computed with magics against the current
        // occupancy rather than read from bitboards::kchecks, which holds
        // empty-board slider attacks. On a real board a bishop sitting on a
        // kchecks square usually is not giving check at all, because
        // something stands in between, so that table overstates slider checks
        // badly and is only correct for the knight.
        //
        // "Safe" excludes squares occupied by the attacker's own pieces, which
        // it cannot move onto, and any square in our attack map. The king's
        // own ring counts as defended: a check the king can simply capture is
        // not the danger this term is about.
        {
            const U64 our_attacks = ei.pe->attacks[c] | ei.piece_attacks[c][knight] |
                                    ei.piece_attacks[c][bishop] | ei.piece_attacks[c][rook] |
                                    ei.piece_attacks[c][queen] | ei.kmask[c];
            const U64 safe = ~ei.pieces[them] & ~our_attacks;

            const U64 rook_from = magics::attacks<rook>(ei.all_pieces, s);
            const U64 bishop_from = magics::attacks<bishop>(ei.all_pieces, s);

            U64 check_from[5]{};
            check_from[knight] = bitboards::nmask[s];
            check_from[bishop] = bishop_from;
            check_from[rook] = rook_from;
            check_from[queen] = rook_from | bishop_from;

            for (Piece pc = knight; pc <= queen; ++pc) {
                const U64 checks = check_from[pc] & ei.piece_attacks[them][pc] & safe;
                if (checks)
                    score -= params_.safe_check_weight[pc] * bits::count(checks);
            }
        }

        if (unsafe_bb) {
            mvs &= ~unsafe_bb;

            // Quadratic king safety: concentrated attacks are exponentially dangerous
            int danger_score = 0;
            for (int j = 1; j < 5; ++j)
                danger_score +=
                    static_cast<int>(ei.kattackers[them][j]) * params_.attacker_weight[j];

            score -= (danger_score * danger_score) / params_.king_danger_divisor;

            score += params_.king_safe_sqs[std::min(7, bits::count(mvs))];

            // Attack combinations
            for (Piece p1 = knight; p1 <= queen; ++p1) {
                for (Piece p2 = pawn; p2 < p1; ++p2) {
                    U64 twiceAttacked = ei.kattk_points[them][p1] & ei.kattk_points[them][p2];
                    if (twiceAttacked) {
                        int attack_penalty = params_.attack_combos[p1][p2];
                        score -= attack_penalty;

                        // Any twice-attacked square beside the king that only
                        // the king defends is the dangerous one, so look at all
                        // of them.
                        //
                        // This used to test a single square, pop_lsb of the
                        // set, which is the lowest square index rather than any
                        // chess property. Mirroring the board reverses the rank
                        // order, so the mirrored position picked a different
                        // square out of the same set and reached a different
                        // verdict: the extra 3 * attack_penalty landed on one
                        // color and not the other, worth 12cp with the rook and
                        // knight combination weight of 4.
                        //
                        // twiceAttacked only ever holds squares in our own
                        // king's mask, and attackers_of2 counts the king, so
                        // masking the king out is what makes "undefended"
                        // mean undefended by anything else.
                        U64 remaining = twiceAttacked;
                        while (remaining) {
                            const Square attacked_sq = Square(bits::pop_lsb(remaining));
                            if ((p.attackers_of2(attacked_sq, c) & ~sq_bb) == 0ULL) {
                                score -= 3 * attack_penalty;
                                break;
                            }
                        }
                    }
                }
            }
        }

        // Pawn shelter, weighted towards the middlegame.
        {
            // Computed live rather than read from the pawn hash. The hash is
            // keyed on pawn structure alone, so a mask built from the king
            // square does not belong in it: castling leaves the pawns
            // untouched, hits the same entry, and would score the shelter
            // against wherever the king stood when that entry was filled.
            U64 pawn_shelter = p.get_pieces<c, pawn>() & ei.kmask[c];
            int n = std::min(3, bits::count(pawn_shelter));
            int shelter = params_.king_shelter[n] / 2;

            // Pawnless flank penalty
            U64 kflank = bitboards::kflanks[util::col(s)] & p.get_pieces<c, pawn>();
            if (!kflank)
                shelter -= 2;

            score += (shelter * mg) / material_entry::kPhaseMax;
        }



        // Enemy pawn storm
        {
            auto pawnStormMask = bitboards::kpawnstorm[c][!(util::col(s) >= Col::E)];
            auto pawnStorm = pawnStormMask & enemyPawns;
            auto numAttackers = bits::count(pawnStorm);
            int storm = 0;
            if (numAttackers >= 2) {
                storm -= 2;
                if (numAttackers >= 3)
                    storm -= 2;
            }
            score += (storm * mg) / material_entry::kPhaseMax;
        }
    }
    return score;
}

// ─── eval_space ─────────────────────────────────────────────────────────────

template <Color c> int HCEEvaluator::eval_space(const position& p, einfo& ei) {
    int score = 0;
    // Space is worth having because pieces need somewhere to go, so it fades
    // as the pieces do. Returning zero the moment the material matched a
    // classified ending made it worth full value right up to that boundary and
    // nothing after it.
    const int mg = ei.me->mg_weight();
    if (mg == 0)
        return score;

    U64 spacemask =
        (bitboards::row[r3] | bitboards::row[r4] | bitboards::row[r5] | bitboards::row[r6]) |
        (bitboards::col[C] | bitboards::col[D] | bitboards::col[E]);

    U64 pawns = p.get_pieces<c, pawn>();
    U64 doubled = ei.pe->doubled[c];
    U64 isolated = ei.pe->isolated[c];
    pawns &= ~(doubled | isolated);
    pawns &= spacemask;

    U64 space = 0ULL;
    while (pawns) {
        int s = bits::pop_lsb(pawns);
        space |= util::squares_behind(bitboards::col[util::col(s)], c, s);
    }
    score += bits::count(space);
    return (score * mg) / material_entry::kPhaseMax;
}

// ─── eval_threats ───────────────────────────────────────────────────────────

template <Color c> int HCEEvaluator::eval_threats(const position& p, einfo& ei) {
    int score = 0;
    constexpr Color them = Color(c ^ 1);
    auto pawnAttacks = ei.pe->attacks[c];
    auto enemyPawnAttacks = ei.pe->attacks[them];
    auto enemyPawns = p.get_pieces<them, pawn>();
    auto enemies = ei.pieces[them] ^ enemyPawns;
    auto ourPieceAttacks = (ei.piece_attacks[c][knight] | ei.piece_attacks[c][bishop] |
                            ei.piece_attacks[c][rook] | ei.piece_attacks[c][queen]);
    auto enemyPieceAttacks = (ei.piece_attacks[them][knight] | ei.piece_attacks[them][bishop] |
                              ei.piece_attacks[them][rook] | ei.piece_attacks[them][queen]);

    // 1. Pieces under attack by pawns
    auto attackedByPawns = enemies & pawnAttacks;
    if (attackedByPawns != 0ULL)
        score += 1;

    // 2. Hanging pieces under attack
    auto defendendEnemies = enemies & (enemyPawnAttacks | enemyPieceAttacks);
    auto undefendendEnemies = enemies ^ defendendEnemies;
    while (undefendendEnemies) {
        auto to = Square(bits::pop_lsb(undefendendEnemies));
        auto victim = p.piece_on(to);
        auto sqbb = bitboards::squares[to];
        if (sqbb & ei.piece_attacks[c][knight])
            score += 2 * params_.attack_scaling[knight] * params_.knight_attks[victim];
        if (sqbb & ei.piece_attacks[c][bishop])
            score += 2 * params_.attack_scaling[bishop] * params_.bishop_attks[victim];
        if (sqbb & ei.piece_attacks[c][rook])
            score += 2 * params_.attack_scaling[rook] * params_.rook_attks[victim];
        if (sqbb & ei.piece_attacks[c][queen])
            score += 2 * params_.attack_scaling[queen] * params_.queen_attks[victim];
    }

    // 3. Hanging weak pawns
    auto weakPawns = ei.weak_pawns[them];
    auto defendendWeakPawns = weakPawns & (enemyPawnAttacks | enemyPieceAttacks);
    auto undefendendWeakPawns = weakPawns ^ defendendWeakPawns;
    if (undefendendWeakPawns) {
        if (undefendendWeakPawns & ei.piece_attacks[c][knight])
            score += bits::count(undefendendWeakPawns & ei.piece_attacks[c][knight]);
        if (undefendendWeakPawns & ei.piece_attacks[c][bishop])
            score += bits::count(undefendendWeakPawns & ei.piece_attacks[c][bishop]);
        if (undefendendWeakPawns & ei.piece_attacks[c][rook])
            score += bits::count(undefendendWeakPawns & ei.piece_attacks[c][rook]);
        if (undefendendWeakPawns & ei.piece_attacks[c][queen])
            score += bits::count(undefendendWeakPawns & ei.piece_attacks[c][queen]);
    }

    // 4. Pieces pinned to queen
    auto enemyQueens = p.get_pieces<them, queen>();
    auto ourRooks = p.get_pieces<c, rook>();
    auto ourBishops = p.get_pieces<c, bishop>();
    while (enemyQueens) {
        auto queenSq = bits::pop_lsb(enemyQueens);
        auto rookPinners = bitboards::rattks[queenSq] & ourRooks;
        auto bishopPinners = bitboards::battks[queenSq] & ourBishops;
        while (rookPinners) {
            auto rookSq = bits::pop_lsb(rookPinners);
            auto betweenMask = bitboards::between[rookSq][queenSq];
            auto pinnedByRook = (enemies & betweenMask) ^ bitboards::squares[queenSq];
            if (pinnedByRook && bits::count(pinnedByRook) == 1) {
                auto pp = p.piece_on(Square(bits::pop_lsb(pinnedByRook)));
                if (pp == bishop || pp == knight)
                    score += 6;
            }
        }
        while (bishopPinners) {
            auto bishopSq = bits::pop_lsb(bishopPinners);
            auto betweenMask = bitboards::between[bishopSq][queenSq];
            auto pinnedByBishop = (enemies & betweenMask) ^ bitboards::squares[queenSq];
            if (pinnedByBishop && bits::count(pinnedByBishop) == 1) {
                auto pp = p.piece_on(Square(bits::pop_lsb(pinnedByBishop)));
                if (pp == knight)
                    score += 6;
                if (pp == rook)
                    score += 18;
            }
        }
    }

    // 5. Discovered checks
    auto hasDiscovery = false;
    auto ourQueens = p.get_pieces<c, queen>();
    auto enemyKing = p.king_square(them);
    auto rookCheckers = bitboards::rattks[enemyKing] & ourRooks;
    auto bishopCheckers = bitboards::battks[enemyKing] & ourBishops;
    auto ourKnights = p.get_pieces<c, knight>();
    while (bishopCheckers && !hasDiscovery) {
        auto bishopSq = bits::pop_lsb(bishopCheckers);
        auto between = bitboards::between[bishopSq][enemyKing] & (ourRooks | ourKnights);
        if (between && bits::count(between) == 1) {
            hasDiscovery = true;
            score += 10;
        }
    }
    while (rookCheckers && !hasDiscovery) {
        auto rookSq = bits::pop_lsb(rookCheckers);
        auto between = bitboards::between[rookSq][enemyKing] & (ourBishops | ourKnights);
        if (between && bits::count(between) == 1) {
            hasDiscovery = true;
            score += 10;
        }
    }
    auto queenCheckers = (bitboards::rattks[enemyKing] | bitboards::battks[enemyKing]) & ourQueens;
    while (queenCheckers && !hasDiscovery) {
        auto queenSq = bits::pop_lsb(queenCheckers);
        auto between = bitboards::between[queenSq][enemyKing] & ourKnights;
        if (between && bits::count(between) == 1) {
            hasDiscovery = true;
            score += 10;
        }
    }

    // 6. Restriction
    auto ourAttacks = pawnAttacks | ourPieceAttacks;
    auto theirAttacks = enemyPawnAttacks | enemyPieceAttacks;
    score += bits::count(ourAttacks) - bits::count(theirAttacks);

    // 7. Skewer detection
    bishopCheckers = bitboards::battks[enemyKing] & ourBishops;
    auto enemyKnights = p.get_pieces<them, knight>();
    auto enemyBishops = p.get_pieces<them, bishop>();
    auto enemyRooks = p.get_pieces<them, rook>();
    auto enemyQueensAll = p.get_pieces<them, queen>();
    auto enemyPieces = enemyKnights | enemyBishops | enemyRooks | enemyQueensAll;
    auto enemyKingSq = bitboards::squares[enemyKing];
    while (bishopCheckers) {
        auto bishopSq = bits::pop_lsb(bishopCheckers);
        U64 ep_copy = enemyPieces;
        while (ep_copy) {
            auto enemy = bits::pop_lsb(ep_copy);
            auto between = bitboards::between[bishopSq][enemy] & enemyKingSq;
            if (between && bits::count(between) == 1) {
                auto pp = p.piece_on(Square(enemy));
                if (pp == bishop || pp == knight)
                    score += 4;
                if (pp == rook)
                    score += 6;
                if (pp == queen)
                    score += 8;
            }
        }
    }

    return score;
}

// ─── eval_passed_pawns ──────────────────────────────────────────────────────

template <Color c> int HCEEvaluator::eval_passed_pawns(const position& p, einfo& ei) {
    int score = 0;
    U64 passers = ei.pe->passed[c];
    if (passers == 0ULL)
        return score;

    while (passers) {
        Square f = Square(bits::pop_lsb(passers));
        int row_dist = (c == white ? 7 - util::row(f) : util::row(f));

        if (row_dist > 3 || row_dist <= 0) {
            score += params_.passed_pawn_rank_bonus[0];
            continue;
        }

        Square front = (c == white ? Square(f + 8) : Square(f - 8));

        // 1. Is next square blocked?
        if (p.piece_on(front) == no_piece)
            score += params_.passed_pawn_unblocked;

        // 2. Control of front square
        U64 our_attackers = 0ULL;
        U64 their_attackers = 0ULL;
        auto crudeControl = 0;

        if (util::on_board(front)) {
            our_attackers = p.attackers_of2(front, c);
            their_attackers = p.attackers_of2(front, Color(c ^ 1));
        }

        if (our_attackers != 0ULL) {
            crudeControl += bits::count(our_attackers);
            score += params_.passed_pawn_control * bits::count(our_attackers);
        }
        if (their_attackers != 0ULL) {
            crudeControl -= bits::count(their_attackers);
            score -= params_.passed_pawn_control * bits::count(their_attackers);
        }

        // 3. Rooks behind passed pawns
        auto rooks_bb = p.get_pieces<c, rook>();
        if (rooks_bb) {
            while (rooks_bb) {
                auto rf = Square(bits::pop_lsb(rooks_bb));
                if (util::col(rf) == util::col(f)) {
                    auto rowDiff = util::row(rf) - util::row(f);
                    auto isBehind = (c == white ? rowDiff < 0 : rowDiff > 0);
                    auto supports = ((bitboards::between[rf][f] & p.all_pieces()) ^
                                     (bitboards::squares[rf] | bitboards::squares[f])) == 0ULL;
                    if (isBehind)
                        score += params_.passed_pawn_rook_behind;
                    if (isBehind && supports) {
                        crudeControl += 1;
                        score += params_.passed_pawn_rook_support;
                    }
                }
            }
        }

        // 4. Connected passers
        auto connectedPassed = (bitboards::neighbor_cols[util::col(f)] & ei.pe->passed[c]) != 0ULL;
        if (connectedPassed)
            score += params_.passed_pawn_connected;

        // 5. Closer to promotion
        score += params_.passed_pawn_rank_bonus[4 - row_dist];

        if (crudeControl < 0)
            score -= params_.passed_pawn_blocked_penalty[3 - row_dist];
    }
    return score;
}

// ─── trapped_rook ───────────────────────────────────────────────────────────

template <Color c> bool HCEEvaluator::trapped_rook(const position& p, einfo& /*ei*/, Square rs) {
    int ks = p.king_square(c);
    int kcol = util::col(ks);
    int krow = util::row(ks);
    int rcol = util::col(rs);
    int rrow = util::row(rs);

    if (krow != rrow)
        return false;
    if (krow != (c == white ? Row::r1 : Row::r8))
        return false;
    if ((kcol < Col::E) != (rcol < kcol))
        return false;

    return true;
}

// ─── Endgame helpers ────────────────────────────────────────────────────────

template <Color c> bool HCEEvaluator::has_opposition(const position& p, einfo& /*ei*/) {
    Square wks = p.king_square(white);
    Square bks = p.king_square(black);
    Color tmv = p.to_move();

    int cols = util::col_dist(wks, bks) - 1;
    int rows = util::row_dist(wks, bks) - 1;
    bool odd_rows = ((rows & 1) == 1);
    bool odd_cols = ((cols & 1) == 1);

    // distant opposition
    if (cols > 0 && rows > 0)
        return (tmv != c && odd_rows && odd_cols);
    // direct opposition
    return (tmv != c && (odd_rows || odd_cols));
}

template <Color c>
float HCEEvaluator::eval_passed_kpk(const position& p, einfo& /*ei*/, Square f, bool has_opp) {
    float score = 0;
    constexpr float advanced_passed_pawn_bonus = 15;
    constexpr float good_king_bonus = 5;
    constexpr Color them = Color(c ^ 1);

    Square ks = p.king_square(c);
    int row_ks = util::row(ks);

    Square eks = p.king_square(them);
    int row_eks = util::row(eks);
    int col_eks = util::col(eks);

    int row = util::row(f);
    int col = util::col(f);

    U64 eks_bb = bitboards::kmask[f] & bitboards::squares[eks];
    U64 fks_bb = bitboards::kmask[f] & bitboards::squares[ks];

    bool e_control_next = eks_bb != 0ULL;
    bool f_control_next = fks_bb != 0ULL;
    bool f_king_infront = (c == white ? row_ks >= row : row_ks <= row);
    bool e_king_infront = (c == white ? row_eks > row : row_eks < row);

    // Edge column draw
    if (col == Col::A || col == Col::H) {
        if (e_control_next)
            return 0;
        if (col_eks == col && e_king_infront)
            return 0;
    }

    // Bad king position
    if (e_king_infront && !f_king_infront && e_control_next && !has_opp)
        return 0;

    // We control front square and have opposition
    if (f_control_next && has_opp)
        score += good_king_bonus;

    int dist = (c == white ? 7 - row : row);
    bool inside_pawn_box = util::col_dist(eks, f) <= dist;

    int fk_dist = std::max(util::col_dist(ks, f), util::row_dist(ks, f));
    int ek_dist = std::max(util::col_dist(eks, f), util::row_dist(eks, f));
    bool too_far = fk_dist >= ek_dist;
    if (too_far && !f_king_infront && inside_pawn_box)
        return 0;

    // Distance to queening bonus
    switch (dist) {
    case 0:
        score += 7 * advanced_passed_pawn_bonus;
        break;
    case 1:
        score += 6 * advanced_passed_pawn_bonus;
        break;
    case 2:
        score += 5 * advanced_passed_pawn_bonus;
        break;
    case 3:
        score += 4 * advanced_passed_pawn_bonus;
        break;
    case 4:
        score += 3 * advanced_passed_pawn_bonus;
        break;
    case 5:
        score += 2 * advanced_passed_pawn_bonus;
        break;
    case 6:
        score += 1 * advanced_passed_pawn_bonus;
        break;
    default:
        break;
    }

    return score;
}

template <Color c>
float HCEEvaluator::eval_passed_krrk(const position& p, einfo& /*ei*/, Square f, bool /*has_opp*/) {
    float score = 0;
    constexpr float advanced_passed_pawn_bonus = 15;
    constexpr float rook_behind_pawn_bonus = 8;
    constexpr Color them = Color(c ^ 1);

    Square eks = p.king_square(them);

    int row = util::row(f);
    int col = util::col(f);

    Square frs = p.squares_of<c, rook>()[0];
    int col_fr = util::col(frs);
    int row_fr = util::row(frs);

    // Rook behind passer
    bool fr_behind = (c == white ? row_fr < row : row_fr > row);
    if (fr_behind) {
        score += rook_behind_pawn_bonus;
        if (col_fr == col)
            score += rook_behind_pawn_bonus;
    }

    // Inactive enemy king
    auto kingPawnDist = std::max(util::row_dist(eks, f), util::col_dist(eks, f));
    score += kingPawnDist;

    // Distance to queening
    int dist = (c == white ? 7 - row : row);
    switch (dist) {
    case 1:
        score += 6 * advanced_passed_pawn_bonus;
        break;
    case 2:
        score += 5 * advanced_passed_pawn_bonus;
        break;
    case 3:
        score += 4 * advanced_passed_pawn_bonus;
        break;
    case 4:
        score += 3 * advanced_passed_pawn_bonus;
        break;
    case 5:
        score += 2 * advanced_passed_pawn_bonus;
        break;
    case 6:
        score += 1 * advanced_passed_pawn_bonus;
        break;
    default:
        break;
    }

    return score;
}

template <Color c>
float HCEEvaluator::eval_passed_knbk(const position& p, einfo& /*ei*/, Square f, bool /*has_opp*/) {
    float score = 0;
    constexpr float advanced_passed_pawn_bonus = 2;
    constexpr float same_bishop_as_queen_sq_bonus = 2;
    constexpr float blockade_penalty = 2;
    constexpr Color them = Color(c ^ 1);

    Square eks = p.king_square(them);

    int row = util::row(f);
    int col = util::col(f);
    Square frontSquare = Square(c == white ? f + 8 : f - 8);
    Square bishopSquare = p.squares_of<c, bishop>()[0];
    auto hasBishop = bishopSquare != no_square;

    // Inactive enemy king
    auto kingPawnDist = std::max(util::row_dist(eks, f), util::col_dist(eks, f));
    score += kingPawnDist;

    if (hasBishop) {
        auto bishopSquareBB = bitboards::squares[bishopSquare];
        auto lightSqBishop = (bishopSquareBB & bitboards::colored_sqs[white]) != 0ULL;
        auto queenSquare = bitboards::squares[(c == white ? col + 56 : col)];
        auto lightQueenSq = (bitboards::colored_sqs[white] & queenSquare) != 0ULL;
        if ((lightQueenSq && lightSqBishop) || (!lightSqBishop && !lightQueenSq))
            score += same_bishop_as_queen_sq_bonus;

        // Knight blockade
        auto frontSquareLight =
            (bitboards::squares[frontSquare] & bitboards::colored_sqs[white]) != 0ULL;
        Square knightSquare = p.squares_of<them, knight>()[0];
        if (knightSquare == frontSquare) {
            score -= blockade_penalty;
            if ((frontSquareLight && !lightSqBishop) || (!frontSquareLight && lightSqBishop)) {
                score -= blockade_penalty;
                if (kingPawnDist <= 2)
                    score -= blockade_penalty;
            }
        }

        // Inactive enemy knight
        auto knightPawnDist =
            std::max(util::row_dist(knightSquare, f), util::col_dist(knightSquare, f));
        score += knightPawnDist;
    }

    // Distance to queening
    int dist = (c == white ? 7 - row : row);
    switch (dist) {
    case 1:
        score += 6 * advanced_passed_pawn_bonus;
        break;
    case 2:
        score += 5 * advanced_passed_pawn_bonus;
        break;
    case 3:
        score += 4 * advanced_passed_pawn_bonus;
        break;
    case 4:
        score += 3 * advanced_passed_pawn_bonus;
        break;
    case 5:
        score += 2 * advanced_passed_pawn_bonus;
        break;
    case 6:
        score += 1 * advanced_passed_pawn_bonus;
        break;
    default:
        break;
    }

    return score;
}

// ─── eval_kpk ───────────────────────────────────────────────────────────────

template <Color c> int HCEEvaluator::eval_kpk(const position& p, einfo& ei) {
    int score = 0;
    constexpr int opposition_bonus = 4;
    constexpr int pawn_spread_bonus = 2;

    bool has_opp = has_opposition<c>(p, ei);

    // Pawns on both sides
    auto queensidePawns = (ei.pe->queenside[c] & (bitboards::col[Col::A] | bitboards::col[Col::B] |
                                                  bitboards::col[Col::C])) != 0ULL;
    auto kingsidePawns = (ei.pe->kingside[c] & (bitboards::col[Col::H] | bitboards::col[Col::G] |
                                                bitboards::col[Col::F])) != 0ULL;
    if (queensidePawns && kingsidePawns)
        score += pawn_spread_bonus;

    // Passed pawns
    U64 passed_pawns = ei.pe->passed[c];
    while (passed_pawns) {
        int f = bits::pop_lsb(passed_pawns);
        score += static_cast<int>(eval_passed_kpk<c>(p, ei, Square(f), has_opp));
    }

    // Opposition bonus
    if (has_opp)
        score += opposition_bonus;

    return score;
}

// ─── eval_krrk ──────────────────────────────────────────────────────────────

template <Color c> int HCEEvaluator::eval_krrk(const position& p, einfo& ei) {
    int score = 0;
    constexpr int opposition_bonus = 2;
    constexpr int pawn_spread_bonus = 4;

    bool has_opp = has_opposition<c>(p, ei);
    if (has_opp)
        score += opposition_bonus;

    // Pawns on both sides
    auto queensidePawns = (ei.pe->queenside[c] & (bitboards::col[Col::A] | bitboards::col[Col::B] |
                                                  bitboards::col[Col::C])) != 0ULL;
    auto kingsidePawns = (ei.pe->kingside[c] & (bitboards::col[Col::H] | bitboards::col[Col::G] |
                                                bitboards::col[Col::F])) != 0ULL;
    if (queensidePawns && kingsidePawns)
        score += pawn_spread_bonus;

    // Passed pawns
    U64 passed_pawns = ei.pe->passed[c];
    while (passed_pawns) {
        int f = bits::pop_lsb(passed_pawns);
        score += static_cast<int>(eval_passed_krrk<c>(p, ei, Square(f), has_opp));
    }
    return score;
}

// ─── eval_knbk ──────────────────────────────────────────────────────────────

template <Color c> int HCEEvaluator::eval_knbk(const position& p, einfo& ei) {
    int score = 0;
    constexpr int opposition_bonus = 2;
    constexpr int pawn_spread_bonus = 4;

    bool has_opp = has_opposition<c>(p, ei);
    if (has_opp)
        score += opposition_bonus;

    // Pawns on both sides with bishop
    auto bishops = p.get_pieces<c, bishop>();
    auto hasBishop = (bishops != 0ULL);
    if (hasBishop) {
        auto queensidePawns =
            (ei.pe->queenside[c] &
             (bitboards::col[Col::A] | bitboards::col[Col::B] | bitboards::col[Col::C])) != 0ULL;
        auto kingsidePawns =
            (ei.pe->kingside[c] &
             (bitboards::col[Col::H] | bitboards::col[Col::G] | bitboards::col[Col::F])) != 0ULL;
        if (queensidePawns && kingsidePawns)
            score += pawn_spread_bonus;

        // Reward pawns on opposite color as bishop
        U64 bishops_copy = bishops;
        auto lightSqBishop = (bitboards::squares[bits::pop_lsb(bishops_copy)] &
                              bitboards::colored_sqs[white]) != 0ULL;
        if (lightSqBishop)
            score += bits::count(ei.pe->dark[c]);
        else
            score += bits::count(ei.pe->light[c]);
    }

    // Passed pawns
    U64 passed_pawns = ei.pe->passed[c];
    while (passed_pawns) {
        int f = bits::pop_lsb(passed_pawns);
        score += static_cast<int>(eval_passed_knbk<c>(p, ei, Square(f), has_opp));
    }
    return score;
}

// ─── eval_krk (King + Rook vs King) ─────────────────────────────────────────

template <Color c> int HCEEvaluator::eval_krk(const position& p, einfo& /*ei*/) {
    constexpr Color them = (c == white ? black : white);

    // Only score for the side that has the rook
    if (p.number_of(c, rook) == 0)
        return 0;

    Square enemy_king = p.king_square(them);
    Square our_king = p.king_square(c);
    int ek_row = util::row(enemy_king);
    int ek_col = util::col(enemy_king);

    // Distance from center (higher = on edge = good for us)
    int center_dist = std::max(std::abs(ek_row - 3), std::abs(ek_col - 3));
    // King proximity (closer = better for mating)
    int king_dist = util::row_dist(our_king, enemy_king) + util::col_dist(our_king, enemy_king);

    return 10 * center_dist - 5 * king_dist + 500;
}

// ─── eval_kqk (King + Queen vs King) ────────────────────────────────────────

template <Color c> int HCEEvaluator::eval_kqk(const position& p, einfo& /*ei*/) {
    constexpr Color them = (c == white ? black : white);

    if (p.number_of(c, queen) == 0)
        return 0;

    Square enemy_king = p.king_square(them);
    Square our_king = p.king_square(c);
    int ek_row = util::row(enemy_king);
    int ek_col = util::col(enemy_king);

    // Drive enemy king to edge
    int center_dist = std::max(std::abs(ek_row - 3), std::abs(ek_col - 3));
    int king_dist = util::row_dist(our_king, enemy_king) + util::col_dist(our_king, enemy_king);

    return 15 * center_dist - 5 * king_dist + 900;
}

// ─── eval_kbnk (King + Bishop + Knight vs King) ─────────────────────────────

template <Color c> int HCEEvaluator::eval_kbnk(const position& p, einfo& /*ei*/) {
    constexpr Color them = (c == white ? black : white);

    if (p.number_of(c, bishop) == 0 || p.number_of(c, knight) == 0)
        return 0;

    Square enemy_king = p.king_square(them);
    Square our_king = p.king_square(c);
    Square bishop_sq = p.squares_of<c, bishop>()[0];
    int ek_row = util::row(enemy_king);
    int ek_col = util::col(enemy_king);

    // Determine if bishop is on light or dark square
    bool light_bishop = (bitboards::squares[bishop_sq] & bitboards::colored_sqs[white]) != 0ULL;

    // Drive enemy king to correct corner (same color as bishop)
    // Light bishop → a1(light) or h8(light); Dark bishop → a8(dark) or h1(dark)
    int dist_a1 = std::max(ek_row, ek_col);         // distance to A1 corner
    int dist_h8 = std::max(7 - ek_row, 7 - ek_col); // distance to H8 corner
    int dist_a8 = std::max(7 - ek_row, ek_col);     // distance to A8 corner
    int dist_h1 = std::max(ek_row, 7 - ek_col);     // distance to H1 corner

    int corner_dist;
    if (light_bishop) {
        // Drive to a1 or h8 (light corners)
        corner_dist = std::min(dist_a1, dist_h8);
    } else {
        // Drive to a8 or h1 (dark corners)
        corner_dist = std::min(dist_a8, dist_h1);
    }

    int king_dist = util::row_dist(our_king, enemy_king) + util::col_dist(our_king, enemy_king);

    // Penalty for enemy king being far from correct corner, bonus for close kings
    return 20 * (3 - corner_dist) - 5 * king_dist + 400;
}

// ─── Explicit template instantiation ────────────────────────────────────────

template int HCEEvaluator::eval_pawns<white>(const position&, einfo&);
template int HCEEvaluator::eval_pawns<black>(const position&, einfo&);
template int HCEEvaluator::eval_knights<white>(const position&, einfo&);
template int HCEEvaluator::eval_knights<black>(const position&, einfo&);
template int HCEEvaluator::eval_bishops<white>(const position&, einfo&);
template int HCEEvaluator::eval_bishops<black>(const position&, einfo&);
template int HCEEvaluator::eval_rooks<white>(const position&, einfo&);
template int HCEEvaluator::eval_rooks<black>(const position&, einfo&);
template int HCEEvaluator::eval_queens<white>(const position&, einfo&);
template int HCEEvaluator::eval_queens<black>(const position&, einfo&);
template int HCEEvaluator::eval_king<white>(const position&, einfo&);
template int HCEEvaluator::eval_king<black>(const position&, einfo&);
template int HCEEvaluator::eval_space<white>(const position&, einfo&);
template int HCEEvaluator::eval_space<black>(const position&, einfo&);
template int HCEEvaluator::eval_threats<white>(const position&, einfo&);
template int HCEEvaluator::eval_threats<black>(const position&, einfo&);
template int HCEEvaluator::eval_passed_pawns<white>(const position&, einfo&);
template int HCEEvaluator::eval_passed_pawns<black>(const position&, einfo&);
template int HCEEvaluator::eval_kpk<white>(const position&, einfo&);
template int HCEEvaluator::eval_kpk<black>(const position&, einfo&);
template int HCEEvaluator::eval_krrk<white>(const position&, einfo&);
template int HCEEvaluator::eval_krrk<black>(const position&, einfo&);
template int HCEEvaluator::eval_knbk<white>(const position&, einfo&);
template int HCEEvaluator::eval_knbk<black>(const position&, einfo&);
template int HCEEvaluator::eval_krk<white>(const position&, einfo&);
template int HCEEvaluator::eval_krk<black>(const position&, einfo&);
template int HCEEvaluator::eval_kqk<white>(const position&, einfo&);
template int HCEEvaluator::eval_kqk<black>(const position&, einfo&);
template int HCEEvaluator::eval_kbnk<white>(const position&, einfo&);
template int HCEEvaluator::eval_kbnk<black>(const position&, einfo&);
template bool HCEEvaluator::trapped_rook<white>(const position&, einfo&, Square);
template bool HCEEvaluator::trapped_rook<black>(const position&, einfo&, Square);
template bool HCEEvaluator::has_opposition<white>(const position&, einfo&);
template bool HCEEvaluator::has_opposition<black>(const position&, einfo&);
template float HCEEvaluator::eval_passed_kpk<white>(const position&, einfo&, Square, bool);
template float HCEEvaluator::eval_passed_kpk<black>(const position&, einfo&, Square, bool);
template float HCEEvaluator::eval_passed_krrk<white>(const position&, einfo&, Square, bool);
template float HCEEvaluator::eval_passed_krrk<black>(const position&, einfo&, Square, bool);
template float HCEEvaluator::eval_passed_knbk<white>(const position&, einfo&, Square, bool);
template float HCEEvaluator::eval_passed_knbk<black>(const position&, einfo&, Square, bool);

} // namespace havoc
