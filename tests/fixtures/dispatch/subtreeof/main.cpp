// Subtree overflow fixture: BI plus 260 direct implementers make
// the static-type subtree 261 nodes at depth 1 - past the 256-node cap, so
// the narrowing must bail out to the whole family and say so
// (subtree_overflow), never silently drop deep targets.
struct BI
{
    virtual int f() = 0;
    virtual ~BI() = default;
};
struct W0 : BI
{
    int f() override;
};
struct W1 : BI
{
    int f() override;
};
struct W2 : BI
{
    int f() override;
};
struct W3 : BI
{
    int f() override;
};
struct W4 : BI
{
    int f() override;
};
struct W5 : BI
{
    int f() override;
};
struct W6 : BI
{
    int f() override;
};
struct W7 : BI
{
    int f() override;
};
struct W8 : BI
{
    int f() override;
};
struct W9 : BI
{
    int f() override;
};
struct W10 : BI
{
    int f() override;
};
struct W11 : BI
{
    int f() override;
};
struct W12 : BI
{
    int f() override;
};
struct W13 : BI
{
    int f() override;
};
struct W14 : BI
{
    int f() override;
};
struct W15 : BI
{
    int f() override;
};
struct W16 : BI
{
    int f() override;
};
struct W17 : BI
{
    int f() override;
};
struct W18 : BI
{
    int f() override;
};
struct W19 : BI
{
    int f() override;
};
struct W20 : BI
{
    int f() override;
};
struct W21 : BI
{
    int f() override;
};
struct W22 : BI
{
    int f() override;
};
struct W23 : BI
{
    int f() override;
};
struct W24 : BI
{
    int f() override;
};
struct W25 : BI
{
    int f() override;
};
struct W26 : BI
{
    int f() override;
};
struct W27 : BI
{
    int f() override;
};
struct W28 : BI
{
    int f() override;
};
struct W29 : BI
{
    int f() override;
};
struct W30 : BI
{
    int f() override;
};
struct W31 : BI
{
    int f() override;
};
struct W32 : BI
{
    int f() override;
};
struct W33 : BI
{
    int f() override;
};
struct W34 : BI
{
    int f() override;
};
struct W35 : BI
{
    int f() override;
};
struct W36 : BI
{
    int f() override;
};
struct W37 : BI
{
    int f() override;
};
struct W38 : BI
{
    int f() override;
};
struct W39 : BI
{
    int f() override;
};
struct W40 : BI
{
    int f() override;
};
struct W41 : BI
{
    int f() override;
};
struct W42 : BI
{
    int f() override;
};
struct W43 : BI
{
    int f() override;
};
struct W44 : BI
{
    int f() override;
};
struct W45 : BI
{
    int f() override;
};
struct W46 : BI
{
    int f() override;
};
struct W47 : BI
{
    int f() override;
};
struct W48 : BI
{
    int f() override;
};
struct W49 : BI
{
    int f() override;
};
struct W50 : BI
{
    int f() override;
};
struct W51 : BI
{
    int f() override;
};
struct W52 : BI
{
    int f() override;
};
struct W53 : BI
{
    int f() override;
};
struct W54 : BI
{
    int f() override;
};
struct W55 : BI
{
    int f() override;
};
struct W56 : BI
{
    int f() override;
};
struct W57 : BI
{
    int f() override;
};
struct W58 : BI
{
    int f() override;
};
struct W59 : BI
{
    int f() override;
};
struct W60 : BI
{
    int f() override;
};
struct W61 : BI
{
    int f() override;
};
struct W62 : BI
{
    int f() override;
};
struct W63 : BI
{
    int f() override;
};
struct W64 : BI
{
    int f() override;
};
struct W65 : BI
{
    int f() override;
};
struct W66 : BI
{
    int f() override;
};
struct W67 : BI
{
    int f() override;
};
struct W68 : BI
{
    int f() override;
};
struct W69 : BI
{
    int f() override;
};
struct W70 : BI
{
    int f() override;
};
struct W71 : BI
{
    int f() override;
};
struct W72 : BI
{
    int f() override;
};
struct W73 : BI
{
    int f() override;
};
struct W74 : BI
{
    int f() override;
};
struct W75 : BI
{
    int f() override;
};
struct W76 : BI
{
    int f() override;
};
struct W77 : BI
{
    int f() override;
};
struct W78 : BI
{
    int f() override;
};
struct W79 : BI
{
    int f() override;
};
struct W80 : BI
{
    int f() override;
};
struct W81 : BI
{
    int f() override;
};
struct W82 : BI
{
    int f() override;
};
struct W83 : BI
{
    int f() override;
};
struct W84 : BI
{
    int f() override;
};
struct W85 : BI
{
    int f() override;
};
struct W86 : BI
{
    int f() override;
};
struct W87 : BI
{
    int f() override;
};
struct W88 : BI
{
    int f() override;
};
struct W89 : BI
{
    int f() override;
};
struct W90 : BI
{
    int f() override;
};
struct W91 : BI
{
    int f() override;
};
struct W92 : BI
{
    int f() override;
};
struct W93 : BI
{
    int f() override;
};
struct W94 : BI
{
    int f() override;
};
struct W95 : BI
{
    int f() override;
};
struct W96 : BI
{
    int f() override;
};
struct W97 : BI
{
    int f() override;
};
struct W98 : BI
{
    int f() override;
};
struct W99 : BI
{
    int f() override;
};
struct W100 : BI
{
    int f() override;
};
struct W101 : BI
{
    int f() override;
};
struct W102 : BI
{
    int f() override;
};
struct W103 : BI
{
    int f() override;
};
struct W104 : BI
{
    int f() override;
};
struct W105 : BI
{
    int f() override;
};
struct W106 : BI
{
    int f() override;
};
struct W107 : BI
{
    int f() override;
};
struct W108 : BI
{
    int f() override;
};
struct W109 : BI
{
    int f() override;
};
struct W110 : BI
{
    int f() override;
};
struct W111 : BI
{
    int f() override;
};
struct W112 : BI
{
    int f() override;
};
struct W113 : BI
{
    int f() override;
};
struct W114 : BI
{
    int f() override;
};
struct W115 : BI
{
    int f() override;
};
struct W116 : BI
{
    int f() override;
};
struct W117 : BI
{
    int f() override;
};
struct W118 : BI
{
    int f() override;
};
struct W119 : BI
{
    int f() override;
};
struct W120 : BI
{
    int f() override;
};
struct W121 : BI
{
    int f() override;
};
struct W122 : BI
{
    int f() override;
};
struct W123 : BI
{
    int f() override;
};
struct W124 : BI
{
    int f() override;
};
struct W125 : BI
{
    int f() override;
};
struct W126 : BI
{
    int f() override;
};
struct W127 : BI
{
    int f() override;
};
struct W128 : BI
{
    int f() override;
};
struct W129 : BI
{
    int f() override;
};
struct W130 : BI
{
    int f() override;
};
struct W131 : BI
{
    int f() override;
};
struct W132 : BI
{
    int f() override;
};
struct W133 : BI
{
    int f() override;
};
struct W134 : BI
{
    int f() override;
};
struct W135 : BI
{
    int f() override;
};
struct W136 : BI
{
    int f() override;
};
struct W137 : BI
{
    int f() override;
};
struct W138 : BI
{
    int f() override;
};
struct W139 : BI
{
    int f() override;
};
struct W140 : BI
{
    int f() override;
};
struct W141 : BI
{
    int f() override;
};
struct W142 : BI
{
    int f() override;
};
struct W143 : BI
{
    int f() override;
};
struct W144 : BI
{
    int f() override;
};
struct W145 : BI
{
    int f() override;
};
struct W146 : BI
{
    int f() override;
};
struct W147 : BI
{
    int f() override;
};
struct W148 : BI
{
    int f() override;
};
struct W149 : BI
{
    int f() override;
};
struct W150 : BI
{
    int f() override;
};
struct W151 : BI
{
    int f() override;
};
struct W152 : BI
{
    int f() override;
};
struct W153 : BI
{
    int f() override;
};
struct W154 : BI
{
    int f() override;
};
struct W155 : BI
{
    int f() override;
};
struct W156 : BI
{
    int f() override;
};
struct W157 : BI
{
    int f() override;
};
struct W158 : BI
{
    int f() override;
};
struct W159 : BI
{
    int f() override;
};
struct W160 : BI
{
    int f() override;
};
struct W161 : BI
{
    int f() override;
};
struct W162 : BI
{
    int f() override;
};
struct W163 : BI
{
    int f() override;
};
struct W164 : BI
{
    int f() override;
};
struct W165 : BI
{
    int f() override;
};
struct W166 : BI
{
    int f() override;
};
struct W167 : BI
{
    int f() override;
};
struct W168 : BI
{
    int f() override;
};
struct W169 : BI
{
    int f() override;
};
struct W170 : BI
{
    int f() override;
};
struct W171 : BI
{
    int f() override;
};
struct W172 : BI
{
    int f() override;
};
struct W173 : BI
{
    int f() override;
};
struct W174 : BI
{
    int f() override;
};
struct W175 : BI
{
    int f() override;
};
struct W176 : BI
{
    int f() override;
};
struct W177 : BI
{
    int f() override;
};
struct W178 : BI
{
    int f() override;
};
struct W179 : BI
{
    int f() override;
};
struct W180 : BI
{
    int f() override;
};
struct W181 : BI
{
    int f() override;
};
struct W182 : BI
{
    int f() override;
};
struct W183 : BI
{
    int f() override;
};
struct W184 : BI
{
    int f() override;
};
struct W185 : BI
{
    int f() override;
};
struct W186 : BI
{
    int f() override;
};
struct W187 : BI
{
    int f() override;
};
struct W188 : BI
{
    int f() override;
};
struct W189 : BI
{
    int f() override;
};
struct W190 : BI
{
    int f() override;
};
struct W191 : BI
{
    int f() override;
};
struct W192 : BI
{
    int f() override;
};
struct W193 : BI
{
    int f() override;
};
struct W194 : BI
{
    int f() override;
};
struct W195 : BI
{
    int f() override;
};
struct W196 : BI
{
    int f() override;
};
struct W197 : BI
{
    int f() override;
};
struct W198 : BI
{
    int f() override;
};
struct W199 : BI
{
    int f() override;
};
struct W200 : BI
{
    int f() override;
};
struct W201 : BI
{
    int f() override;
};
struct W202 : BI
{
    int f() override;
};
struct W203 : BI
{
    int f() override;
};
struct W204 : BI
{
    int f() override;
};
struct W205 : BI
{
    int f() override;
};
struct W206 : BI
{
    int f() override;
};
struct W207 : BI
{
    int f() override;
};
struct W208 : BI
{
    int f() override;
};
struct W209 : BI
{
    int f() override;
};
struct W210 : BI
{
    int f() override;
};
struct W211 : BI
{
    int f() override;
};
struct W212 : BI
{
    int f() override;
};
struct W213 : BI
{
    int f() override;
};
struct W214 : BI
{
    int f() override;
};
struct W215 : BI
{
    int f() override;
};
struct W216 : BI
{
    int f() override;
};
struct W217 : BI
{
    int f() override;
};
struct W218 : BI
{
    int f() override;
};
struct W219 : BI
{
    int f() override;
};
struct W220 : BI
{
    int f() override;
};
struct W221 : BI
{
    int f() override;
};
struct W222 : BI
{
    int f() override;
};
struct W223 : BI
{
    int f() override;
};
struct W224 : BI
{
    int f() override;
};
struct W225 : BI
{
    int f() override;
};
struct W226 : BI
{
    int f() override;
};
struct W227 : BI
{
    int f() override;
};
struct W228 : BI
{
    int f() override;
};
struct W229 : BI
{
    int f() override;
};
struct W230 : BI
{
    int f() override;
};
struct W231 : BI
{
    int f() override;
};
struct W232 : BI
{
    int f() override;
};
struct W233 : BI
{
    int f() override;
};
struct W234 : BI
{
    int f() override;
};
struct W235 : BI
{
    int f() override;
};
struct W236 : BI
{
    int f() override;
};
struct W237 : BI
{
    int f() override;
};
struct W238 : BI
{
    int f() override;
};
struct W239 : BI
{
    int f() override;
};
struct W240 : BI
{
    int f() override;
};
struct W241 : BI
{
    int f() override;
};
struct W242 : BI
{
    int f() override;
};
struct W243 : BI
{
    int f() override;
};
struct W244 : BI
{
    int f() override;
};
struct W245 : BI
{
    int f() override;
};
struct W246 : BI
{
    int f() override;
};
struct W247 : BI
{
    int f() override;
};
struct W248 : BI
{
    int f() override;
};
struct W249 : BI
{
    int f() override;
};
struct W250 : BI
{
    int f() override;
};
struct W251 : BI
{
    int f() override;
};
struct W252 : BI
{
    int f() override;
};
struct W253 : BI
{
    int f() override;
};
struct W254 : BI
{
    int f() override;
};
struct W255 : BI
{
    int f() override;
};
struct W256 : BI
{
    int f() override;
};
struct W257 : BI
{
    int f() override;
};
struct W258 : BI
{
    int f() override;
};
struct W259 : BI
{
    int f() override;
};

int W0::f() { return 0; }
int W1::f() { return 1; }
int W2::f() { return 2; }
int W3::f() { return 3; }
int W4::f() { return 4; }
int W5::f() { return 5; }
int W6::f() { return 6; }
int W7::f() { return 7; }
int W8::f() { return 8; }
int W9::f() { return 9; }
int W10::f() { return 10; }
int W11::f() { return 11; }
int W12::f() { return 12; }
int W13::f() { return 13; }
int W14::f() { return 14; }
int W15::f() { return 15; }
int W16::f() { return 16; }
int W17::f() { return 17; }
int W18::f() { return 18; }
int W19::f() { return 19; }
int W20::f() { return 20; }
int W21::f() { return 21; }
int W22::f() { return 22; }
int W23::f() { return 23; }
int W24::f() { return 24; }
int W25::f() { return 25; }
int W26::f() { return 26; }
int W27::f() { return 27; }
int W28::f() { return 28; }
int W29::f() { return 29; }
int W30::f() { return 30; }
int W31::f() { return 31; }
int W32::f() { return 32; }
int W33::f() { return 33; }
int W34::f() { return 34; }
int W35::f() { return 35; }
int W36::f() { return 36; }
int W37::f() { return 37; }
int W38::f() { return 38; }
int W39::f() { return 39; }
int W40::f() { return 40; }
int W41::f() { return 41; }
int W42::f() { return 42; }
int W43::f() { return 43; }
int W44::f() { return 44; }
int W45::f() { return 45; }
int W46::f() { return 46; }
int W47::f() { return 47; }
int W48::f() { return 48; }
int W49::f() { return 49; }
int W50::f() { return 50; }
int W51::f() { return 51; }
int W52::f() { return 52; }
int W53::f() { return 53; }
int W54::f() { return 54; }
int W55::f() { return 55; }
int W56::f() { return 56; }
int W57::f() { return 57; }
int W58::f() { return 58; }
int W59::f() { return 59; }
int W60::f() { return 60; }
int W61::f() { return 61; }
int W62::f() { return 62; }
int W63::f() { return 63; }
int W64::f() { return 64; }
int W65::f() { return 65; }
int W66::f() { return 66; }
int W67::f() { return 67; }
int W68::f() { return 68; }
int W69::f() { return 69; }
int W70::f() { return 70; }
int W71::f() { return 71; }
int W72::f() { return 72; }
int W73::f() { return 73; }
int W74::f() { return 74; }
int W75::f() { return 75; }
int W76::f() { return 76; }
int W77::f() { return 77; }
int W78::f() { return 78; }
int W79::f() { return 79; }
int W80::f() { return 80; }
int W81::f() { return 81; }
int W82::f() { return 82; }
int W83::f() { return 83; }
int W84::f() { return 84; }
int W85::f() { return 85; }
int W86::f() { return 86; }
int W87::f() { return 87; }
int W88::f() { return 88; }
int W89::f() { return 89; }
int W90::f() { return 90; }
int W91::f() { return 91; }
int W92::f() { return 92; }
int W93::f() { return 93; }
int W94::f() { return 94; }
int W95::f() { return 95; }
int W96::f() { return 96; }
int W97::f() { return 97; }
int W98::f() { return 98; }
int W99::f() { return 99; }
int W100::f() { return 100; }
int W101::f() { return 101; }
int W102::f() { return 102; }
int W103::f() { return 103; }
int W104::f() { return 104; }
int W105::f() { return 105; }
int W106::f() { return 106; }
int W107::f() { return 107; }
int W108::f() { return 108; }
int W109::f() { return 109; }
int W110::f() { return 110; }
int W111::f() { return 111; }
int W112::f() { return 112; }
int W113::f() { return 113; }
int W114::f() { return 114; }
int W115::f() { return 115; }
int W116::f() { return 116; }
int W117::f() { return 117; }
int W118::f() { return 118; }
int W119::f() { return 119; }
int W120::f() { return 120; }
int W121::f() { return 121; }
int W122::f() { return 122; }
int W123::f() { return 123; }
int W124::f() { return 124; }
int W125::f() { return 125; }
int W126::f() { return 126; }
int W127::f() { return 127; }
int W128::f() { return 128; }
int W129::f() { return 129; }
int W130::f() { return 130; }
int W131::f() { return 131; }
int W132::f() { return 132; }
int W133::f() { return 133; }
int W134::f() { return 134; }
int W135::f() { return 135; }
int W136::f() { return 136; }
int W137::f() { return 137; }
int W138::f() { return 138; }
int W139::f() { return 139; }
int W140::f() { return 140; }
int W141::f() { return 141; }
int W142::f() { return 142; }
int W143::f() { return 143; }
int W144::f() { return 144; }
int W145::f() { return 145; }
int W146::f() { return 146; }
int W147::f() { return 147; }
int W148::f() { return 148; }
int W149::f() { return 149; }
int W150::f() { return 150; }
int W151::f() { return 151; }
int W152::f() { return 152; }
int W153::f() { return 153; }
int W154::f() { return 154; }
int W155::f() { return 155; }
int W156::f() { return 156; }
int W157::f() { return 157; }
int W158::f() { return 158; }
int W159::f() { return 159; }
int W160::f() { return 160; }
int W161::f() { return 161; }
int W162::f() { return 162; }
int W163::f() { return 163; }
int W164::f() { return 164; }
int W165::f() { return 165; }
int W166::f() { return 166; }
int W167::f() { return 167; }
int W168::f() { return 168; }
int W169::f() { return 169; }
int W170::f() { return 170; }
int W171::f() { return 171; }
int W172::f() { return 172; }
int W173::f() { return 173; }
int W174::f() { return 174; }
int W175::f() { return 175; }
int W176::f() { return 176; }
int W177::f() { return 177; }
int W178::f() { return 178; }
int W179::f() { return 179; }
int W180::f() { return 180; }
int W181::f() { return 181; }
int W182::f() { return 182; }
int W183::f() { return 183; }
int W184::f() { return 184; }
int W185::f() { return 185; }
int W186::f() { return 186; }
int W187::f() { return 187; }
int W188::f() { return 188; }
int W189::f() { return 189; }
int W190::f() { return 190; }
int W191::f() { return 191; }
int W192::f() { return 192; }
int W193::f() { return 193; }
int W194::f() { return 194; }
int W195::f() { return 195; }
int W196::f() { return 196; }
int W197::f() { return 197; }
int W198::f() { return 198; }
int W199::f() { return 199; }
int W200::f() { return 200; }
int W201::f() { return 201; }
int W202::f() { return 202; }
int W203::f() { return 203; }
int W204::f() { return 204; }
int W205::f() { return 205; }
int W206::f() { return 206; }
int W207::f() { return 207; }
int W208::f() { return 208; }
int W209::f() { return 209; }
int W210::f() { return 210; }
int W211::f() { return 211; }
int W212::f() { return 212; }
int W213::f() { return 213; }
int W214::f() { return 214; }
int W215::f() { return 215; }
int W216::f() { return 216; }
int W217::f() { return 217; }
int W218::f() { return 218; }
int W219::f() { return 219; }
int W220::f() { return 220; }
int W221::f() { return 221; }
int W222::f() { return 222; }
int W223::f() { return 223; }
int W224::f() { return 224; }
int W225::f() { return 225; }
int W226::f() { return 226; }
int W227::f() { return 227; }
int W228::f() { return 228; }
int W229::f() { return 229; }
int W230::f() { return 230; }
int W231::f() { return 231; }
int W232::f() { return 232; }
int W233::f() { return 233; }
int W234::f() { return 234; }
int W235::f() { return 235; }
int W236::f() { return 236; }
int W237::f() { return 237; }
int W238::f() { return 238; }
int W239::f() { return 239; }
int W240::f() { return 240; }
int W241::f() { return 241; }
int W242::f() { return 242; }
int W243::f() { return 243; }
int W244::f() { return 244; }
int W245::f() { return 245; }
int W246::f() { return 246; }
int W247::f() { return 247; }
int W248::f() { return 248; }
int W249::f() { return 249; }
int W250::f() { return 250; }
int W251::f() { return 251; }
int W252::f() { return 252; }
int W253::f() { return 253; }
int W254::f() { return 254; }
int W255::f() { return 255; }
int W256::f() { return 256; }
int W257::f() { return 257; }
int W258::f() { return 258; }
int W259::f() { return 259; }

int probe_overflow()
{
    BI* p = nullptr;
    return p->f();
}

int main() { return 0; }
