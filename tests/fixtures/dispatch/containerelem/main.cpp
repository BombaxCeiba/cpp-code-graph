// Container element slot fixture: std container
// locals/fields get a "#elem:" slot at declaration time (Declared, static
// element type only); write-set calls (push_back/insert/assign), copy
// flows (copy-construct / copy-assign / assign(begin, end)) and range-for
// link rows carry the dynamic evidence to the loop variable. The smart
// pointer shell peels through the operator-> canonical single criterion
// (zero name matching); a plain array keeps its honest no-dynamic fallback.

#include <vector>

struct Sink
{
    virtual ~Sink() = default;
    virtual int flow() = 0;
};
struct FileSink : Sink
{
    int flow() override { return 1; }
};
struct MemSink : Sink
{
    int flow() override { return 2; }
};

// Self-owned smart pointer shell: pattern operator-> returns T*, so the
// canonical pointee spelling carries the type-parameter- marker (single
// criterion, no names involved).
template <typename T>
struct Box
{
    T* ptr_;
    explicit Box(T* p) : ptr_(p) {}
    T* operator->() { return ptr_; }
};

// Same-function write then read: single dynamic type reaches 0.7.
int probe_loop()
{
    std::vector<Sink*> sinks;
    sinks.push_back(new FileSink);
    for (auto* s : sinks)
    {
        return s->flow();
    }
    return 0;
}

// Multiple writes: union of dynamic types caps the slot at low (0.4 x2).
int probe_multi()
{
    std::vector<Sink*> sinks;
    sinks.push_back(new FileSink);
    sinks.push_back(new MemSink);
    for (auto* s : sinks)
    {
        return s->flow();
    }
    return 0;
}

// Copy-construct local copy flow: dst's element slot inherits src's.
int probe_copy()
{
    std::vector<Sink*> src;
    src.push_back(new MemSink);
    std::vector<Sink*> dst = src;
    for (auto* s : dst)
    {
        return s->flow();
    }
    return 0;
}

// Copy-assignment flow into a fresh local container.
int probe_copy_assign()
{
    std::vector<Sink*> src;
    src.push_back(new FileSink);
    std::vector<Sink*> dst;
    dst = src;
    for (auto* s : dst)
    {
        return s->flow();
    }
    return 0;
}

// Field container: element slot owned by the declaring record; the
// write-set lands on the record slot and the range-for link row resolves
// through the @fld descriptor.
struct SinkHost
{
    std::vector<Sink*> sinks_;
    void               fill() { sinks_.push_back(new MemSink); }
    int                drain_field()
    {
        sinks_.push_back(new FileSink);
        for (auto* s : sinks_)
        {
            return s->flow();
        }
        return 0;
    }
    // Iterator-range assign copy flow: local snapshot inherits the field
    // element slot (a real-world sink-slot shape).
    int drain_snapshot()
    {
        std::vector<Sink*> snap;
        snap.assign(sinks_.begin(), sinks_.end());
        for (auto* s : snap)
        {
            return s->flow();
        }
        return 0;
    }
};

// Smart pointer element container: element type peels the shell, the
// push_back argument peels through the shell's T* constructor.
int probe_shell()
{
    std::vector<Box<Sink>> boxes;
    boxes.push_back(Box<Sink>(new FileSink));
    for (Box<Sink> b : boxes)
    {
        return b->flow();
    }
    return 0;
}

// Negative control: a plain array is not a std container, no element slot,
// no link row - the site keeps its honest no_dynamic_type_evidence.
int probe_array(Sink* (&arr)[2])
{
    for (auto* s : arr)
    {
        return s->flow();
    }
    return 0;
}

int main()
{
    SinkHost host;
    host.fill();
    const int a = probe_loop();
    const int b = probe_multi();
    const int c = probe_copy();
    const int d = probe_copy_assign();
    const int e = host.drain_field();
    const int f = host.drain_snapshot();
    const int g = probe_shell();
    Sink*     arr[2] = {new FileSink, new MemSink};
    const int h = probe_array(arr);
    return a + b + c + d + e + f + g + h;
}
