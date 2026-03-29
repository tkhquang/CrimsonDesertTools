## Fixed equipment hash IDs for game v1.01.00

- Fix equipment hide not working on game v1.01.00 — the patch inserted ~60 new entries (bags, accessories) into the IndexedStringA table, shifting all 98 equipment part hash IDs upward and causing most categories (1H weapons, shields, bows, special weapons, lanterns) to fail silently while tools and 2H weapons appeared to work by coincidence
- Update range filter bounds from `0xAD00-0xBFFF` to `0xAE03-0xBFFF` and outlier IDs from `0x0F4E`/`0x12435` to `0x0F6D`/`0x12A79`
