#include <vector>
#include <algorithm>
#include <cstring>
#include <chrono>
#include <iostream>
#include <fstream>
#include <thread>
#include <mutex>
#include <atomic>
#include <cstdarg>
#include <iomanip>
#include <sstream>
#include <ctime>
#include <random>
#include <cmath>

#ifdef _WIN32
#define DLLEXPORT extern "C" __declspec(dllexport)
#else
#define DLLEXPORT extern "C"
#endif

const int TT_SIZE = 1 << 24; 
const int INF = 1000000;
const int MATE = 900000;
const int MAX_PLY = 64;

const int EMPTY = 0;
const int BLACK_MAN = 1;
const int WHITE_MAN = 2;
const int BLACK_KING = 3;
const int WHITE_KING = 4;
const int GHOST = 7; 

struct Step { int r1, c1, r2, c2; };
struct MoveResult { Step steps[12]; int count; int score; int depth; int nodes; };
struct Move { 
    Step steps[12]; int count; int score; 
    bool operator>(const Move& other) const { return score > other.score; }
    bool operator==(const Move& other) const {
        if (count != other.count) return false;
        if (count == 0) return true;
        return steps[0].r1 == other.steps[0].r1 && steps[0].c1 == other.steps[0].c1 &&
               steps[count-1].r2 == other.steps[count-1].r2 && steps[count-1].c2 == other.steps[count-1].c2;
    }
};

struct MoveList {
    Move moves[128];
    int count = 0;
    void push_back(const Move& m) { if (count < 128) moves[count++] = m; }
    void clear() { count = 0; }
    Move& operator[](int index) { return moves[index]; }
    const Move& operator[](int index) const { return moves[index]; }
};

std::mutex log_mutex;
std::string current_log_filename;
bool log_initialized = false;
unsigned long long zobrist_table[64][8]; 
unsigned long long zobrist_black_move;
int history_table[2][64][64]; 
Move killer_moves[MAX_PLY][2]; 
std::chrono::time_point<std::chrono::high_resolution_clock> start_time;
double time_limit_sec;
bool stop_search = false;
long long nodes_visited = 0;

std::string move_to_str(const Move& m);

void init_log_filename() {
    if (log_initialized) return;
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << "waylon_" << std::put_time(std::localtime(&in_time_t), "%Y-%m-%d_%H-%M-%S") << ".log";
    current_log_filename = ss.str();
    log_initialized = true;
}

void log_file(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(log_mutex);
    if (!log_initialized) init_log_filename();
    std::ofstream logfile(current_log_filename, std::ios::app);
    if (logfile.is_open()) {
        char buffer[8192];
        va_list args; va_start(args, fmt); vsnprintf(buffer, sizeof(buffer), fmt, args); va_end(args);
        logfile << buffer << "\n";
    }
}
void log_final_move(const Move& m, int player, int score, int depth) {
    log_file("FINAL BEST MOVE (Player %d, Score %d, Depth %d): %s", player, score, depth, move_to_str(m).c_str());
}

std::string board_to_str(const int board[64]) {
    std::stringstream ss;
    ss << "\n   +-----------------+\n";
    for(int r=0; r<8; r++) {
        ss << " " << r << " | ";
        for(int c=0; c<8; c++) {
            int p = board[r*8+c];
            char sym = '.';
            if (p == BLACK_MAN) sym = 'b'; else if (p == WHITE_MAN) sym = 'w';
            else if (p == BLACK_KING) sym = 'B'; else if (p == WHITE_KING) sym = 'W';
            ss << sym << " ";
        }
        ss << "|\n";
    }
    ss << "   +-----------------+\n     0 1 2 3 4 5 6 7\n";
    return ss.str();
}

void init_zobrist() {
    std::mt19937_64 rng(12345);
    for(int i=0; i<64; i++) for(int j=0; j<8; j++) zobrist_table[i][j] = rng();
    zobrist_black_move = rng();
    std::memset(history_table, 0, sizeof(history_table));
}

unsigned long long compute_hash(const int board[64], int player) {
    unsigned long long h = 0;
    for(int i=0; i<64; i++) {
        if(board[i]!=EMPTY && board[i]!=GHOST) h ^= zobrist_table[i][board[i]];
    }
    if(player == BLACK_MAN || player == BLACK_KING) h ^= zobrist_black_move;
    return h;
}

enum TTFlag { TT_EXACT, TT_ALPHA, TT_BETA };
struct TTEntry { unsigned long long key; int score; int depth; int flag; Move best_move; };
TTEntry* transposition_table = nullptr;

void init_memory() {
    if (transposition_table == nullptr) {
        transposition_table = new TTEntry[TT_SIZE];
        std::memset(transposition_table, 0, TT_SIZE * sizeof(TTEntry));
        init_zobrist();
    }
}

void tt_save(unsigned long long key, int score, int depth, int flag, const Move& best_move) {
    int idx = key % TT_SIZE;
    if (transposition_table[idx].key != key || depth >= transposition_table[idx].depth) {
        transposition_table[idx] = {key, score, depth, flag, best_move};
    }
}

bool tt_probe(unsigned long long key, int depth, int alpha, int beta, int& score, Move& tt_move) {
    int idx = key % TT_SIZE;
    if (transposition_table[idx].key == key) {
        tt_move = transposition_table[idx].best_move;
        if (transposition_table[idx].depth >= depth) {
            int s = transposition_table[idx].score;
            if (s > MATE - 1000) s -= (MAX_PLY); 
            if (s < -MATE + 1000) s += (MAX_PLY);
            
            if (transposition_table[idx].flag == TT_EXACT) { score = s; return true; }
            if (transposition_table[idx].flag == TT_ALPHA && s <= alpha) { score = alpha; return true; }
            if (transposition_table[idx].flag == TT_BETA && s >= beta) { score = beta; return true; }
        }
    }
    return false;
}

static const int R[64] = {
    0,0,0,0,0,0,0,0, 1,1,1,1,1,1,1,1, 2,2,2,2,2,2,2,2, 3,3,3,3,3,3,3,3,
    4,4,4,4,4,4,4,4, 5,5,5,5,5,5,5,5, 6,6,6,6,6,6,6,6, 7,7,7,7,7,7,7,7
};
static const int C[64] = {
    0,1,2,3,4,5,6,7, 0,1,2,3,4,5,6,7, 0,1,2,3,4,5,6,7, 0,1,2,3,4,5,6,7,
    0,1,2,3,4,5,6,7, 0,1,2,3,4,5,6,7, 0,1,2,3,4,5,6,7, 0,1,2,3,4,5,6,7
};

inline bool is_valid(int r, int c) { return r >= 0 && r < 8 && c >= 0 && c < 8; }

int evaluate(const int board[64], int current_player) {
    int w_score = 0, b_score = 0;
    
    const int VAL_PAWN = 1000;
    const int VAL_KING = 8000; 
    
    const int RUNAWAY_BONUS = 600; 
    const int MOBILITY_WEIGHT = 6; 

    static const int PST_WHITE[64] = {
        0, 0, 0, 0, 0, 0, 0, 0,          // R0 (King = 8000)
        1500, 1500, 1500, 1500, 1500, 1500, 1500, 1500, // R1
        800, 800, 800, 800, 800, 800, 800, 800,  // R2
        200, 200, 250, 250, 250, 250, 200, 200,  // R3
        100, 100, 150, 150, 150, 150, 100, 100,  // R4
        50,  50,  80,  80,  80,  80,  50,  50,   // R5
        20,  20,  20,  20,  20,  20,  20,  20,   // R6
        10,  10,  10,  10,  10,  10,  10,  10    // R7 (Base)
    };

    static const int PST_BLACK[64] = {
        10,  10,  10,  10,  10,  10,  10,  10,   // R0 (Base)
        20,  20,  20,  20,  20,  20,  20,  20,   // R1
        50,  50,  80,  80,  80,  80,  50,  50,   // R2
        100, 100, 150, 150, 150, 150, 100, 100,  // R3
        200, 200, 250, 250, 250, 250, 200, 200,  // R4
        800, 800, 800, 800, 800, 800, 800, 800,  // R5
        1500, 1500, 1500, 1500, 1500, 1500, 1500, 1500, // R6
        0, 0, 0, 0, 0, 0, 0, 0                   // R7 (King = 8000)
    };

    int w_men = 0, b_men = 0;
    int w_kings = 0, b_kings = 0;
    int w_moves = 0, b_moves = 0;

    for(int i=0; i<64; i++) {
        int p = board[i];
        if (p == EMPTY || p == GHOST) continue;

        int r = R[i]; int c = C[i];

        if (p == WHITE_MAN) {
            w_men++;
            w_score += VAL_PAWN;
            w_score += PST_WHITE[i];
            
            if (r <= 2) w_score += RUNAWAY_BONUS;

            if (r > 0) {
                if (c > 0 && board[i-9] == EMPTY) w_moves++;
                if (c < 7 && board[i-7] == EMPTY) w_moves++;
            }
        } 
        else if (p == BLACK_MAN) {
            b_men++;
            b_score += VAL_PAWN;
            b_score += PST_BLACK[i];
            
            if (r >= 5) b_score += RUNAWAY_BONUS;

            if (r < 7) {
                if (c > 0 && board[i+7] == EMPTY) b_moves++;
                if (c < 7 && board[i+9] == EMPTY) b_moves++;
            }
        } 
        else if (p == WHITE_KING) { 
            w_kings++; w_score += VAL_KING; w_moves += 5; 
        }
        else if (p == BLACK_KING) { 
            b_kings++; b_score += VAL_KING; b_moves += 5; 
        }
    }
    
    const int DANGER_NEAR_KING = 1000; 

    for(int i=48; i<56; i++) {
        if (board[i] == BLACK_MAN) {
            bool safe = false;
            if (is_valid(7, C[i]-1) && board[i+7] != EMPTY) safe = true;
            if (is_valid(7, C[i]+1) && board[i+9] != EMPTY) safe = true;
            if (!safe) w_score -= DANGER_NEAR_KING;
        }
    }
    for(int i=8; i<16; i++) {
        if (board[i] == WHITE_MAN) {
            bool safe = false;
            if (is_valid(0, C[i]-1) && board[i-9] != EMPTY) safe = true;
            if (is_valid(0, C[i]+1) && board[i-7] != EMPTY) safe = true;
            if (!safe) b_score -= DANGER_NEAR_KING;
        }
    }

    w_score += w_moves * MOBILITY_WEIGHT;
    b_score += b_moves * MOBILITY_WEIGHT;

    int w_total = w_men + w_kings;
    int b_total = b_men + b_kings;
    if (w_total > b_total) w_score += (2500 / (b_total + 1));
    if (b_total > w_total) b_score += (2500 / (w_total + 1));

    int score = w_score - b_score;
    return (current_player == WHITE_MAN || current_player == WHITE_KING) ? score : -score;
}

void find_captures_recursive(int board[64], int r, int c, int piece, Move current_move, MoveList& moves) {
    if (current_move.count >= 12) {
        moves.push_back(current_move);
        return;
    }
    
    bool is_white = (piece == WHITE_MAN || piece == WHITE_KING);
    bool is_king = (piece == WHITE_KING || piece == BLACK_KING);
    
    int my_man = is_white ? WHITE_MAN : BLACK_MAN;
    int my_king = is_white ? WHITE_KING : BLACK_KING;
    int enemy_man = is_white ? BLACK_MAN : WHITE_MAN;
    int enemy_king = is_white ? BLACK_KING : WHITE_KING;
    
    int drs[] = {-1, -1, 1, 1}; int dcs[] = {-1, 1, -1, 1}; 
    
    bool found_continuation = false;
    for (int i = 0; i < 4; ++i) {
        int dr = drs[i]; int dc = dcs[i];
        
        if (!is_king) {
            int nr = r + dr; int nc = c + dc;
            int jr = r + 2 * dr; int jc = c + 2 * dc;
            if (is_valid(jr, jc)) {
                int mid = board[nr*8+nc];
                if ((mid == enemy_man || mid == enemy_king) && board[jr*8+jc] == EMPTY) {
                    int temp_board[64]; std::memcpy(temp_board, board, 64*sizeof(int));
                    temp_board[r*8+c] = EMPTY; 
                    temp_board[nr*8+nc] = GHOST; 
                    temp_board[jr*8+jc] = piece; 
                    
                    Move next = current_move; next.steps[next.count++] = {r, c, jr, jc};
                    find_captures_recursive(temp_board, jr, jc, piece, next, moves);
                    found_continuation = true;
                }
            }
        } else {
            int dist = 1;
            while(true) {
                int nr = r + dist * dr; int nc = c + dist * dc;
                if (!is_valid(nr, nc)) break;
                
                int p = board[nr*8+nc];
                
                if (p == my_man || p == my_king || p == GHOST) break;

                if (p == enemy_man || p == enemy_king) {
                    int land_dist = 1;
                    while(true) {
                        int jr = nr + land_dist * dr; int jc = nc + land_dist * dc;
                        if (!is_valid(jr, jc)) break;
                        
                        if (board[jr*8+jc] != EMPTY) break;
                        
                        int temp_board[64]; std::memcpy(temp_board, board, 64*sizeof(int));
                        temp_board[r*8+c] = EMPTY; 
                        temp_board[nr*8+nc] = GHOST;
                        temp_board[jr*8+jc] = piece;
                        
                        Move next = current_move; next.steps[next.count++] = {r, c, jr, jc};
                        find_captures_recursive(temp_board, jr, jc, piece, next, moves);
                        
                        found_continuation = true; 
                        land_dist++;
                    }
                    break;
                }
                dist++;
            }
        }
    }
    
    if (!found_continuation && current_move.count > 0) moves.push_back(current_move);
}

void generate_moves(int board[64], int player, MoveList& moves) {
    moves.clear();
    bool is_white = (player == WHITE_MAN || player == WHITE_KING);
    int my_man = is_white ? WHITE_MAN : BLACK_MAN;
    int my_king = is_white ? WHITE_KING : BLACK_KING;
    
    for(int i=0; i<64; i++) {
        if (board[i] == my_man || board[i] == my_king) {
            Move m; m.count = 0; m.score = 0;
            find_captures_recursive(board, i/8, i%8, board[i], m, moves);
        }
    }
    if (moves.count > 0) {
        int max_capture_count = 0;
        for(int i=0; i<moves.count; i++) if (moves[i].count > max_capture_count) max_capture_count = moves[i].count;
        int write_idx = 0;
        for(int i=0; i<moves.count; i++) if (moves[i].count == max_capture_count) moves[write_idx++] = moves[i];
        moves.count = write_idx;
        return; 
    }
    for(int r=0; r<8; r++) {
        for(int c=0; c<8; c++) {
            int p = board[r*8+c];
            if (p != my_man && p != my_king) continue;
            if (p == my_man) {
                int drs[] = { is_white ? -1 : 1, is_white ? -1 : 1 }; int dcs[] = { -1, 1 };
                for(int k=0; k<2; k++) {
                    int nr = r + drs[k]; int nc = c + dcs[k];
                    if (is_valid(nr, nc) && board[nr*8+nc] == EMPTY) {
                        Move m; m.count = 1; m.steps[0] = {r, c, nr, nc}; m.score = 0; moves.push_back(m);
                    }
                }
            } else {
                int drs[] = {-1, -1, 1, 1}; int dcs[] = {-1, 1, -1, 1};
                for(int k=0; k<4; k++) {
                    int dist = 1;
                    while(true) {
                        int nr = r + dist * drs[k]; int nc = c + dist * dcs[k];
                        if (!is_valid(nr, nc) || board[nr*8+nc] != EMPTY) break;
                        Move m; m.count = 1; m.steps[0] = {r, c, nr, nc}; m.score = 0; moves.push_back(m);
                        dist++;
                    }
                }
            }
        }
    }
}

int score_move_ordering(const Move& m, const Move& tt_best, int ply) {
    if (m == tt_best) return 2000000;
    if (m.count > 0 && std::abs(m.steps[0].r1 - m.steps[m.count-1].r2) > 2) return 1000000 + m.count * 1000;
    
    bool promotion = false;
    if (m.steps[m.count-1].r2 == 0 || m.steps[m.count-1].r2 == 7) promotion = true;
    if (promotion) return 950000;

    if (m == killer_moves[ply][0]) return 900000;
    if (m == killer_moves[ply][1]) return 800000;
    int from = m.steps[0].r1 * 8 + m.steps[0].c1;
    int to = m.steps[m.count-1].r2 * 8 + m.steps[m.count-1].c2;
    return history_table[0][from][to]; 
}

void pick_move(MoveList& moves, int start_index, const Move& tt_best, int ply) {
    int best_score = -2000000000;
    int best_idx = -1;
    for(int i=start_index; i<moves.count; i++) {
        int s = score_move_ordering(moves[i], tt_best, ply);
        moves[i].score = s; 
        if (s > best_score) { best_score = s; best_idx = i; }
    }
    if (best_idx != -1) std::swap(moves[start_index], moves[best_idx]);
}

void apply_move(int board[64], const Move& m, int player, int& captured_piece_type) {
    int r1 = m.steps[0].r1; int c1 = m.steps[0].c1;
    int r2 = m.steps[m.count-1].r2; int c2 = m.steps[m.count-1].c2;
    int piece = board[r1*8+c1];
    board[r1*8+c1] = EMPTY; board[r2*8+c2] = piece;
    
    if (std::abs(r1 - r2) >= 2 || m.count > 1) { 
         for(int i=0; i<m.count; i++) {
             int sr = m.steps[i].r1; int sc = m.steps[i].c1;
             int er = m.steps[i].r2; int ec = m.steps[i].c2;
             int dr = (er > sr) ? 1 : -1; int dc = (ec > sc) ? 1 : -1;
             int curr_r = sr + dr; int curr_c = sc + dc;
             while(curr_r != er) {
                 if (board[curr_r*8+curr_c] != EMPTY) {
                     captured_piece_type = board[curr_r*8+curr_c]; board[curr_r*8+curr_c] = EMPTY;
                 }
                 curr_r += dr; curr_c += dc;
             }
         }
    }
    bool is_white = (piece == WHITE_MAN); bool is_black = (piece == BLACK_MAN);
    if (is_white && r2 == 0) board[r2*8+c2] = WHITE_KING;
    if (is_black && r2 == 7) board[r2*8+c2] = BLACK_KING;
}

bool is_capture_move(const Move& m) {
    if (m.count == 0) return false;
    return std::abs(m.steps[0].r1 - m.steps[0].r2) > 1; 
}


bool is_promotion_move(const Move& m, int piece) {
    if (m.count != 1) return false; 
    
    int r2 = m.steps[0].r2;
    if (piece == WHITE_MAN && r2 == 0) return true;
    if (piece == BLACK_MAN && r2 == 7) return true;
    return false;
}

int alpha_beta(int board[64], int depth, int alpha, int beta, int player, int ply, bool do_null) {
    nodes_visited++;
    if ((nodes_visited & 2047) == 0) {
        auto now = std::chrono::high_resolution_clock::now();
        if (std::chrono::duration<double>(now - start_time).count() > time_limit_sec) stop_search = true;
    }
    if (stop_search) return 0;

    MoveList moves;
    generate_moves(board, player, moves);
    
    if (moves.count == 0) return -MATE + ply; 

    bool capture_forced = (moves.count > 0 && is_capture_move(moves[0]));
        
    if (depth <= 0) {
        bool is_noisy = capture_forced;
        
        if (!is_noisy) {
             for(int i=0; i<moves.count; i++) {
                 int p = board[moves[i].steps[0].r1 * 8 + moves[i].steps[0].c1];
                 if (is_promotion_move(moves[i], p)) {
                     is_noisy = true;
                     break;
                 }
             }
        }

        if (!is_noisy) {
            return evaluate(board, player);
        }
        
        if (depth < -12) return evaluate(board, player); 
    }

    unsigned long long h = compute_hash(board, player);
    Move tt_move; std::memset(&tt_move, 0, sizeof(Move));
    int tt_score;
    if (tt_probe(h, depth, alpha, beta, tt_score, tt_move)) {
        if (ply > 0) return tt_score; 
    }

    int best_score = -INF;
    Move best_move_local = moves[0];
    int flag = TT_ALPHA;
    int enemy = (player == WHITE_MAN || player == WHITE_KING) ? BLACK_MAN : WHITE_MAN;

    for (int i = 0; i < moves.count; i++) {
        pick_move(moves, i, tt_move, ply);
        const Move& m = moves[i];
        
        int piece = board[m.steps[0].r1 * 8 + m.steps[0].c1];
        bool promotion = is_promotion_move(m, piece);

        int temp_board[64]; std::memcpy(temp_board, board, 64*sizeof(int));
        int captured = 0;
        apply_move(temp_board, m, player, captured);

        int extension = 0;
        if (promotion && ply < MAX_PLY) extension = 1;

        int score;
        if (i == 0) {
            score = -alpha_beta(temp_board, depth - 1 + extension, -beta, -alpha, enemy, ply + 1, true);
        } else {
            int reduction = 0;
            if (depth >= 3 && !capture_forced && !promotion && m.count == 1 && i > 3) reduction = 1;
            
            score = -alpha_beta(temp_board, depth - 1 - reduction + extension, -alpha - 1, -alpha, enemy, ply + 1, true);
            if (score > alpha && score < beta) {
                score = -alpha_beta(temp_board, depth - 1 + extension, -beta, -alpha, enemy, ply + 1, true);
            }
        }
        
        if (stop_search) return 0;

        if (score > best_score) {
            best_score = score;
            best_move_local = m;
            if (score > alpha) {
                alpha = score;
                flag = TT_EXACT;
                if (alpha >= beta) {
                    flag = TT_BETA;
                    if (!capture_forced && m.count == 1) { 
                        killer_moves[ply][1] = killer_moves[ply][0];
                        killer_moves[ply][0] = m;
                        int from = m.steps[0].r1 * 8 + m.steps[0].c1;
                        int to = m.steps[0].r2 * 8 + m.steps[0].c2;
                        history_table[player==WHITE_MAN||player==WHITE_KING?0:1][from][to] += depth * depth;
                    }
                    break; 
                }
            }
        }
    }
    tt_save(h, best_score, depth, flag, best_move_local);
    return best_score;
}

std::string move_to_str(const Move& m) {
    std::stringstream ss;
    for(int i=0; i<m.count; ++i) { 
        if(i>0) ss<<"->"; 
        ss<<"("<<m.steps[i].r1<<","<<m.steps[i].c1<<"-"<<m.steps[i].r2<<","<<m.steps[i].c2<<")"; 
    }
    return ss.str();
}

DLLEXPORT void get_best_move(int* flat_board, int player, double limit_sec, int max_depth, MoveResult* result) {
    init_memory();
    int board[64]; std::memcpy(board, flat_board, 64*sizeof(int));
    
    log_file("%s", board_to_str(board).c_str());

    MoveList root_moves;
    generate_moves(board, player, root_moves);

    if (root_moves.count == 0) { result->count = 0; result->score = -MATE; return; }
    if (root_moves.count == 1) {
        Move best = root_moves[0];
        result->count = best.count; for(int i=0; i<12; i++) result->steps[i] = best.steps[i];
        result->score = 0; result->depth = 1; result->nodes = 0;
        log_file("FORCED MOVE: Only 1 legal move.");
        return;
    }

    start_time = std::chrono::high_resolution_clock::now();
    time_limit_sec = limit_sec;
    stop_search = false;
    nodes_visited = 0;
    
    Move best_overall = root_moves[0]; 
    int best_score_overall = -INF;
    int reached_depth = 0;
    int enemy = (player == WHITE_MAN || player == WHITE_KING) ? BLACK_MAN : WHITE_MAN;

    for (int d = 1; d <= max_depth; d++) {
        int alpha = -INF; int beta = INF;
        pick_move(root_moves, 0, best_overall, 0); 
        Move current_best_move = root_moves[0]; 
        int current_best_score = -INF;
        
        for (int i = 0; i < root_moves.count; i++) {
            const Move& m = root_moves[i];
            int temp_board[64]; std::memcpy(temp_board, board, 64*sizeof(int));
            
            int piece = board[m.steps[0].r1 * 8 + m.steps[0].c1];
            bool promotion = is_promotion_move(m, piece);
            int extension = (promotion) ? 1 : 0;
            
            int captured = 0; apply_move(temp_board, m, player, captured);
            
            int score;
            if (i == 0) score = -alpha_beta(temp_board, d - 1 + extension, -beta, -alpha, enemy, 1, true);
            else {
                score = -alpha_beta(temp_board, d - 1 + extension, -alpha - 1, -alpha, enemy, 1, true);
                if (score > alpha && score < beta) score = -alpha_beta(temp_board, d - 1 + extension, -beta, -alpha, enemy, 1, true);
            }
            if (stop_search) break;
            if (score > current_best_score) {
                current_best_score = score; current_best_move = m;
                if (score > alpha) alpha = score;
            }
        }
        
        if (stop_search) break;

        best_overall = current_best_move;
        best_score_overall = current_best_score;
        reached_depth = d;

        log_file("DEPTH %2d | Score: %6d | Move: %s", d, best_score_overall, move_to_str(best_overall).c_str());
        if (best_score_overall > MATE - 5000) break; 
    }

    result->count = best_overall.count;
    for(int i=0; i<12; i++) result->steps[i] = best_overall.steps[i];
    result->score = best_score_overall;
    result->depth = reached_depth; 
    result->nodes = (int)nodes_visited;
    
    if (result->count == 0 && root_moves.count > 0) {
        result->count = root_moves[0].count; for(int i=0; i<12; i++) result->steps[i] = root_moves[0].steps[i];
    }
    log_final_move(best_overall, player, result->score, reached_depth);
}

int main() { return 0; }