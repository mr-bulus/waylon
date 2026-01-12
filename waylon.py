import sys
import tkinter as tk
from tkinter import messagebox
import time
import threading
import queue
import logging
import traceback
import ctypes
import os
from datetime import datetime

DLL_NAME = "engine.dll"
try:
    if os.path.exists(DLL_NAME):
        cpp_lib = ctypes.CDLL(f"./{DLL_NAME}")
        print(f"Engine loaded: {DLL_NAME}")
    else:
        raise FileNotFoundError(f"{DLL_NAME} not found. Compile engine.cpp!")
except Exception as e:
    messagebox.showerror("Engine error", f"Critical error: {e}")
    sys.exit(1)

# Types definitions for Python
class Step(ctypes.Structure):
    _fields_ = [("r1", ctypes.c_int), ("c1", ctypes.c_int),
                ("r2", ctypes.c_int), ("c2", ctypes.c_int)]

class MoveResult(ctypes.Structure):
    _fields_ = [
        ("steps", Step * 12),
        ("count", ctypes.c_int),
        ("score", ctypes.c_int),
        ("depth", ctypes.c_int),
        ("nodes", ctypes.c_int)
    ]

# C++ function configuration
cpp_lib.get_best_move.argtypes = [
    ctypes.POINTER(ctypes.c_int), # flat board
    ctypes.c_int,                 # player
    ctypes.c_double,              # time limit
    ctypes.c_int,                 # max depth
    ctypes.POINTER(MoveResult)    # result struct
]

MAX_TIME = 5.0       # Thinking time in seconds
MAX_DEPTH_LIMIT = 64 # Maximum depth
MATE_SCORE = 1000000

EMPTY = 0
BLACK_MAN = 1
WHITE_MAN = 2
BLACK_KING = 3
WHITE_KING = 4

class GrandmasterEngine:
    def __init__(self):
        self.nodes = 0

    def find_captures(self, grid, r, c, piece, visited):
        res = []
        is_white = (piece == 2 or piece == 4)
        is_king = (piece == 3 or piece == 4)
        enemy_man = 1 if is_white else 2
        enemy_king = 3 if is_white else 4
        dirs = [(-1, -1), (-1, 1), (1, -1), (1, 1)]
        
        for dr, dc in dirs:
            if not is_king:
                nr, nc = r + dr, c + dc
                jr, jc = r + 2*dr, c + 2*dc
                if 0 <= jr < 8 and 0 <= jc < 8:
                    mid = grid[nr][nc]
                    if (mid == enemy_man or mid == enemy_king) and grid[jr][jc] == 0:
                        if (nr, nc) not in visited:
                            step = [r, c, jr, jc]
                            res.append([step]) 
            else:
                # King logic
                k = 1
                while True:
                    nr, nc = r + k*dr, c + k*dc
                    if not (0 <= nr < 8 and 0 <= nc < 8): break
                    val = grid[nr][nc]
                    if val == 0: k+=1; continue
                    if val == enemy_man or val == enemy_king:
                        # Enemy found
                        land_k = 1
                        while True:
                            jr, jc = nr + land_k*dr, nc + land_k*dc
                            if not (0 <= jr < 8 and 0 <= jc < 8): break
                            if grid[jr][jc] != 0: break
                            if (nr, nc) not in visited:
                                step = [r, c, jr, jc]
                                res.append([step])
                            land_k += 1
                        break
                    else: break
        return res

    def get_legal_moves(self, grid, player):
        moves = []
        captures = []
        
        is_white = (player == 2 or player == 4)
        man = 2 if is_white else 1
        king = 4 if is_white else 3
        
        # 1. Find captures
        for r in range(8):
            for c in range(8):
                if grid[r][c] in (man, king):
                    caps = self.find_captures(grid, r, c, grid[r][c], set())
                    for c_seq in caps:
                        captures.append(c_seq)
        
        if captures:
            # Capture compulsion (only captures are returned)
            return captures, True

        for r in range(8):
            for c in range(8):
                p = grid[r][c]
                if p not in (man, king): continue
                
                is_k = (p == 3 or p == 4)
                dirs = [(-1,-1),(-1,1),(1,-1),(1,1)] if is_k else (
                    [(-1,-1),(-1,1)] if is_white else [(1,-1),(1,1)]
                )
                for dr, dc in dirs:
                    nr, nc = r + dr, c + dc
                    if is_k:
                        while 0 <= nr < 8 and 0 <= nc < 8:
                            if grid[nr][nc] == 0:
                                moves.append([[r, c, nr, nc]])
                                nr += dr; nc += dc
                            else: break
                    else:
                        if 0 <= nr < 8 and 0 <= nc < 8 and grid[nr][nc] == 0:
                            moves.append([[r, c, nr, nc]])           
        return moves, False

    def find_best_move(self, grid_list, player, callback=None, logger=None):
        flat_board = [0] * 64
        for r in range(8):
            for c in range(8):
                flat_board[r*8 + c] = grid_list[r][c]
        
        c_board = (ctypes.c_int * 64)(*flat_board)
        result = MoveResult()
        
        if logger:
            logger.info(f"START SEARCH for Player {player}")

        cpp_lib.get_best_move(c_board, player, MAX_TIME, MAX_DEPTH_LIMIT, ctypes.byref(result))
        
        if result.count == 0:
            return None, -999999

        final_move = []
        for i in range(result.count):
            s = result.steps[i]
            final_move.append([s.r1, s.c1, s.r2, s.c2])
            
        score_txt = str(result.score)
        if result.score > MATE_SCORE - 1000: score_txt = f"MATE +{MATE_SCORE - result.score}"
        elif result.score < -MATE_SCORE + 1000: score_txt = f"MATE -{MATE_SCORE + result.score}"

        log_msg = f"END: Depth={result.depth} Score={score_txt} Nodes={result.nodes}"
        if logger: logger.info(log_msg)
        print(log_msg)
        
        if callback:
            callback(result.depth, score_txt)

        return final_move, result.score, result.depth

    def find_best_move_parallel(self, grid, player, update_callback=None, logger_ref=None):
        return self.find_best_move(grid, player, update_callback, logger_ref)

def setup_logging():
    root_logger = logging.getLogger()
    for h in root_logger.handlers[:]: root_logger.removeHandler(h)
    
    logger = logging.getLogger("System")
    logger.setLevel(logging.INFO)
    sh = logging.StreamHandler(sys.stdout)
    sh.setFormatter(logging.Formatter('%(asctime)s | %(message)s', datefmt='%H:%M:%S'))
    logger.addHandler(sh)
    
    return logger

class CheckersGame:
    def __init__(self, root, logger_ref):
        self.logger = logger_ref
        self.logger.info("=== Waylon GUI v0.1 ===")
        self.root = root
        self.root.title("Waylon GUI")
        self.root.protocol("WM_DELETE_WINDOW", self.on_closing)
        self.is_running = True
        self.gui_queue = queue.Queue()

        self.SQ = 70
        self.board = [[None]*8 for _ in range(8)]
        self.current_player = "black" 
        self.selected = None
        self.valid_moves = []
        self.in_chain = False 
        self.game_over = False
        self.ai_thinking = False
        self.hint_move = None      
        self.turn_counter = 0  
        
        self.engine = GrandmasterEngine()
        
        self.init_board()
        self.build_ui()
        self.root.after(100, self.process_queue)
        self.root.after(200, self.reset_game_logic)

    def on_closing(self):
        self.is_running = False
        self.game_over = True
        try: self.root.destroy()
        except: pass

    def process_queue(self):
        if not self.is_running: return
        try:
            while True:
                msg = self.gui_queue.get_nowait()
                if msg['type'] == 'hint_result':
                    self.display_hint(msg['move'], msg['turn'], msg['depth'], msg.get('score', 0))
                elif msg['type'] == 'depth_update':
                    self.depth_info.config(text=f"D: {msg['depth']} | O: {msg['score']}")
        except queue.Empty: pass
        finally:
            if self.is_running: self.root.after(50, self.process_queue)

    def display_hint(self, move, turn_id, depth, score):
        if not self.is_running or turn_id != self.turn_counter: return
        try:
            self.ai_thinking = False
            self.ai_status.config(text="Ready", fg="green")
            self.depth_info.config(text=f"Final: D={depth} | S={score}")
            if move:
                self.hint_move = move
                self.draw()
        except: pass

    def build_ui(self):
        frame = tk.Frame(self.root)
        frame.pack(side=tk.RIGHT, fill=tk.Y, padx=10)
        self.info = tk.Label(frame, text="...", font=("Arial", 12, "bold"))
        self.info.pack(pady=10)
        self.ai_status = tk.Label(frame, text="Waiting", font=("Arial", 10, "italic"), fg="blue")
        self.ai_status.pack(pady=5)
        self.depth_info = tk.Label(frame, text="Depth: -", font=("Arial", 9), fg="gray")
        self.depth_info.pack(pady=2)
        tk.Button(frame, text="New game", command=self.reset_game_click).pack(pady=10)
        self.canvas = tk.Canvas(self.root, width=8*self.SQ, height=8*self.SQ, bg="#F0D9B5")
        self.canvas.pack(side=tk.LEFT)
        self.canvas.bind("<Button-1>", self.on_click)

    def init_board(self):
        self.board = [[None]*8 for _ in range(8)]
        for r in range(8):
            for c in range(8):
                if (r+c)%2 == 1:
                    if r < 3: self.board[r][c] = {'c': 'black', 'k': False}
                    elif r > 4: self.board[r][c] = {'c': 'white', 'k': False}

    def draw(self):
        if not self.is_running: return
        try:
            self.canvas.delete("all")
            for r in range(8):
                for c in range(8):
                    color = "#F0D9B5" if (r+c)%2==0 else "#B58863"
                    x1, y1, x2, y2 = c*self.SQ, r*self.SQ, (c+1)*self.SQ, (r+1)*self.SQ
                    self.canvas.create_rectangle(x1, y1, x2, y2, fill=color, outline="")
                    if self.selected and (r,c) == self.selected:
                        self.canvas.create_rectangle(x1, y1, x2, y2, fill="#646F40", outline="")
            
            if self.hint_move:
                steps = self.hint_move
                if not isinstance(steps[0], list): steps = [steps]
                
                for step in steps:
                    if len(step) >= 4:
                        x1, y1 = step[1]*self.SQ + self.SQ//2, step[0]*self.SQ + self.SQ//2
                        x2, y2 = step[3]*self.SQ + self.SQ//2, step[2]*self.SQ + self.SQ//2
                        self.canvas.create_line(x1, y1, x2, y2, fill="blue", width=4, arrow=tk.LAST)
                        self.canvas.create_oval(x1-5, y1-5, x1+5, y1+5, fill="blue", outline="")

            for m in self.valid_moves:
                step = m[0]
                if len(step) >= 4:
                    cx, cy = step[3]*self.SQ + self.SQ//2, step[2]*self.SQ + self.SQ//2
                    self.canvas.create_oval(cx-10, cy-10, cx+10, cy+10, fill="lightgreen", outline="green", width=2)

            for r in range(8):
                for c in range(8):
                    p = self.board[r][c]
                    if p:
                        x1, y1, x2, y2 = c*self.SQ+10, r*self.SQ+10, c*self.SQ+self.SQ-10, r*self.SQ+self.SQ-10
                        f_c = "red" if p['c'] == 'white' else "black"
                        o_c = "gold" if p['k'] else ("white" if f_c=="black" else "black")
                        self.canvas.create_oval(x1, y1, x2, y2, fill=f_c, outline=o_c, width=3 if p['k'] else 1)
                        if p['k']: self.canvas.create_text((x1+x2)/2, (y1+y2)/2, text="K", fill="white" if f_c=="black" else "black", font=("Arial", 20, "bold"))
            self.canvas.update_idletasks()
        except Exception as e:
            print(f"Draw error: {e}")

    def on_click(self, e):
        if self.game_over or not self.is_running: return
        r, c = e.y // self.SQ, e.x // self.SQ
        if not (0<=r<8 and 0<=c<8): return
        p = self.board[r][c]
        
        def get_dest(m): 
            return m[0][2], m[0][3]

        if self.in_chain:
            for m in self.valid_moves:
                dr, dc = get_dest(m)
                if dr == r and dc == c: 
                    self.execute_move(m) 
                    return
            return
            
        if p and p['c'] == self.current_player:
            grid = self.board_to_int_grid(self.board)
            pl_int = BLACK_MAN if self.current_player == 'black' else WHITE_MAN
            legal_moves_seqs, _ = self.engine.get_legal_moves(grid, pl_int)
            
            my_moves = []
            for seq in legal_moves_seqs:
                if seq[0][0] == r and seq[0][1] == c:
                    my_moves.append(seq)
            
            if my_moves:
                self.selected = (r, c)
                self.valid_moves = my_moves
            else:
                self.selected = None
                self.valid_moves = []
            self.draw()
            
        elif not p and self.selected:
            for m in self.valid_moves:
                dr, dc = get_dest(m)
                if dr == r and dc == c: 
                    self.execute_move(m) 
                    return

    def execute_move(self, move_seq):
        step = move_seq[0]
        start_r, start_c = step[0], step[1]
        
        p = self.board[start_r][start_c]
        self.board[start_r][start_c] = None
        
        tr, tc = step[2], step[3]
        
        dr = 1 if tr > start_r else -1
        dc = 1 if tc > start_c else -1
        cr, cc = start_r + dr, start_c + dc
        captured = False
        while cr != tr:
            if self.board[cr][cc]: 
                self.board[cr][cc] = None
                captured = True
            cr += dr; cc += dc
            
        self.board[tr][tc] = p
        
        # Chain jump logic
        if captured:
            # Check for further captures from the new position
            grid = self.board_to_int_grid(self.board)
            p_val = grid[tr][tc]
            found = self.engine.find_captures(grid, tr, tc, p_val, set())
            if found:
                self.in_chain = True
                self.selected = (tr, tc)
                self.valid_moves = found
                self.info.config(text=f"Move: {self.current_player} (Go on!)", fg="red")
                self.draw()
                return

        # Promotion
        if p['c'] == 'white' and tr == 0: p['k'] = True
        if p['c'] == 'black' and tr == 7: p['k'] = True
        
        self.in_chain = False
        self.selected = None
        self.valid_moves = []
        self.end_turn()

    def end_turn(self):
        bc = sum(1 for r in range(8) for c in range(8) if self.board[r][c] and self.board[r][c]['c']=='black')
        wc = sum(1 for r in range(8) for c in range(8) if self.board[r][c] and self.board[r][c]['c']=='white')
        if bc == 0:
            messagebox.showinfo("Finish", "Red won!"); self.game_over = True; return
        if wc == 0:
            messagebox.showinfo("Finish", "White won!"); self.game_over = True; return
            
        self.current_player = "black" if self.current_player == "white" else "white"
        self.turn_counter += 1
        self.hint_move = None
        self.update_info()
        self.draw()
        self.start_hint_calculation()

    def update_info(self):
        if not self.is_running: return
        txt = "Blacks (top)" if self.current_player=='black' else "Reds (bottom)"
        try: self.info.config(text=f"Move: {txt}", fg="black")
        except: pass

    def board_to_int_grid(self, ui_board):
        grid = [[0]*8 for _ in range(8)]
        for r in range(8):
            for c in range(8):
                p = ui_board[r][c]
                if p:
                    if p['c'] == 'black': grid[r][c] = BLACK_KING if p['k'] else BLACK_MAN
                    else: grid[r][c] = WHITE_KING if p['k'] else WHITE_MAN
        return grid

    def reset_game_click(self): self.reset_game_logic()
    def reset_game_logic(self):
        self.game_over = False; self.ai_thinking = False; self.hint_move = None; self.in_chain = False
        self.selected = None; self.valid_moves = []; self.turn_counter = 1
        self.init_board(); self.draw()
        if messagebox.askyesno("Select a side", "Reds starting?"): self.current_player = "white"
        else: self.current_player = "black"
        self.update_info(); self.draw(); self.start_hint_calculation()

    def start_hint_calculation(self):
        if self.game_over or not self.is_running: return
        self.ai_thinking = True
        try: self.ai_status.config(text="Thinking...", fg="red")
        except: pass
        threading.Thread(target=self.manager_thread, args=(self.turn_counter,), daemon=True).start()

    def manager_thread(self, tid):
        try:
            grid = self.board_to_int_grid(self.board)
            my_color_int = BLACK_MAN if self.current_player == "black" else WHITE_MAN
            
            def gui_callback(d, s):
                if self.is_running: self.gui_queue.put({'type': 'depth_update', 'depth': d, 'score': s})
            
            best_move, best_score, depth = self.engine.find_best_move_parallel(grid, my_color_int, gui_callback, self.logger)
            
            if self.is_running:
                self.gui_queue.put({'type': 'hint_result', 'move': best_move, 'turn': tid, 'depth': depth, 'score': best_score})
        except Exception as e:
            self.logger.error(f"Manager Error: {e}"); traceback.print_exc()

if __name__ == "__main__":
    logger_ref = setup_logging()
    root = tk.Tk()
    game = CheckersGame(root, logger_ref)
    root.mainloop()