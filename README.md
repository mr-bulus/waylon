I present to you Walyon - a checkers player's assistant.
We play according to the rules of Polish (international) checkers, including:

- capturing a pawn in all directions,
- flying king,
- the rule requiring the capture of the larger row of pawns.

How to get started?

You need a C++ compiler and a Python runtime environment. The code has been prepared to build a DLL.

1. Compile the engine (engine.cpp).
Use, for example, mingw-w64 and then:

g++ -shared -o engine.dll engine.cpp -O3 -static

...or something better.

2. Run:

python waylon.py and have fun!

A few words about the engine.

Search Architecture: The code implements a standard and efficient set of algorithms for games:
- Alpha-Beta pruning with windows,
- Iterative Deepening – allowing for better time utilization and sorting of moves in subsequent iterations,
- Transposition Table (TT) – a hash table for storing previously computed positions,
- Quiescence Search – search expansion to help avoid the horizon effect,
- Late Move Reduction (LMR) – reducing the depth for moves that appear weak.
