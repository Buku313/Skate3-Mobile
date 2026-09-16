package chat.buku.skate3;

/** Public destinations only. Never append local paths, saves, or diagnostics. */
final class ModSubmissionLinks {
    private ModSubmissionLinks() {}

    static String guide(boolean portuguese) {
        return "https://buku313.github.io/Skate3-Mobile/mods/submit.html?lang="
            + (portuguese ? "pt-BR" : "en");
    }

    static String form(boolean portuguese) {
        return "https://github.com/Buku313/Skate3-Mobile/issues/new?template="
            + (portuguese ? "character_mod_submission_pt_br.yml"
                          : "character_mod_submission.yml");
    }
}
