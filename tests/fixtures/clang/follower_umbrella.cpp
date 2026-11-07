// Umbrella twin fixture (MSVC-land mixed TU shape): follower code after
// `import std;` on one physical line (the grown umbrella #include would
// drop it), and a comment-only follower on the next line where the
// directive stays legal.
import std; int umbrella_follow() { return 3; }
import std; // comment-only follower keeps the umbrella include
