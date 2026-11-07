export module fixture.mini;

// Overlay fixture: the shapes a
// named-module interface carries around the export keyword. Parse-input
// DATA for the clang unit tests - never compiled, never indexed (module
// TUs parse only with their build's BMI flags).

export namespace n
{
    int visible_function();

    struct VisibleStruct
    {
        int field;
    };
} // namespace n

export int single_export();

namespace plain
{
    int classic_function();
}
