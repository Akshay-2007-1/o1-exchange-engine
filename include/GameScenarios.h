#pragma once

#include "GameTypes.h"
#include <vector>

// Stub scenario bank - 4 illustrative examples so the game-mode interfaces
// can be reviewed before real content gets built out. Not meant to be the
// final content pipeline (see MS3 game-mode plan, non-goal #1).
inline std::vector<Scenario> default_scenario_bank()
{
    return {
        Scenario{
            1,
            "estimation",
            "How many windows does the Empire State Building have?",
            "windows",
            6514.0,
            {
                "It was completed in 1931.",
                "It has 102 stories above ground.",
                "It's roughly 443 meters tall including the antenna.",
            },
        },
        Scenario{
            2,
            "estimation",
            "How many minutes long is the movie Titanic (1997)?",
            "minutes",
            195.0,
            {
                "It was directed by James Cameron.",
                "It won 11 Academy Awards, including Best Picture.",
                "It's one of the longest mainstream blockbusters ever released.",
            },
        },
        Scenario{
            3,
            "estimation",
            "How many licensed taxi cabs operate in New York City?",
            "taxis",
            13500.0,
            {
                "NYC caps the number of medallion taxis by law.",
                "The cap has stayed roughly the same size for decades.",
                "It's on the order of ten thousand, not one hundred thousand.",
            },
        },
        Scenario{
            4,
            "estimation",
            "How many piano tuners are there in Chicago?",
            "piano tuners",
            150.0,
            {
                "This is a classic Fermi estimation problem.",
                "Try: population, households with pianos, tunings per year, tunings per tuner per year.",
                "Chicago's metro population is on the order of a few million.",
            },
        },
    };
}
