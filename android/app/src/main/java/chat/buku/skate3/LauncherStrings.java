package chat.buku.skate3;

import android.content.Context;

import java.util.HashMap;
import java.util.Locale;
import java.util.Map;

/** Small runtime string layer while the programmatic launcher moves to Android resources. */
final class LauncherStrings {
    static final String ENGLISH = "en";
    static final String PORTUGUESE_BRAZIL = "pt-BR";

    private static final String PREFERENCES = "launcher-language";
    private static final String LANGUAGE = "language";
    private static final Map<String, String> PT = new HashMap<>();

    static {
        put("COMMUNITY MODS", "MODS DA COMUNIDADE");
        put("SUBMIT A MOD", "ENVIAR UM MOD");
        put("Made a character? Send it in for the Mod Store. Character mods are currently supported; every submission is reviewed before publication.",
            "Criou um personagem? Envie para a Loja de Mods. No momento, aceitamos mods de personagens. Todos os envios passam por análise antes da publicação.");
        put("What to prepare", "O que preparar");
        put("1. A ZIP containing base.obj and texture_diffuse.png.\n2. A PNG or JPG preview.\n3. Your creator name, credit link, and redistribution permission.\n4. The app version, tested devices, and any known animation or board-contact problems.",
            "1. Um ZIP com base.obj e texture_diffuse.png.\n2. Uma prévia em PNG ou JPG.\n3. Seu nome de criador, link para os créditos e permissão de redistribuição.\n4. A versão do aplicativo, os dispositivos testados e problemas conhecidos de animação ou contato com o skate.");
        put("Only send work you created or have permission to share. Never attach an ISO, XEX, Title Update, save, or extracted retail game files.",
            "Envie apenas trabalhos que você criou ou tem permissão para compartilhar. Nunca anexe ISO, XEX, Title Update, save ou arquivos extraídos do jogo comercial.");
        put("The form opens on GitHub and requires an account. Submissions are public. You choose the attachments and press Submit on GitHub; this app does not upload files or publish a mod automatically.",
            "O formulário abre no GitHub e exige uma conta. Os envios são públicos. Você escolhe os anexos e confirma o envio no GitHub. Este aplicativo não envia arquivos nem publica mods automaticamente.");
        put("OPEN SUBMISSION FORM", "ABRIR FORMULÁRIO DE ENVIO");
        put("MODEL GUIDE AND SEIYU EXAMPLE", "GUIA DO MODELO E EXEMPLO DO SEIYU");
        put("BACK TO LAUNCHER", "VOLTAR AO LAUNCHER");
        put("No browser available", "Nenhum navegador disponível");
        put("Install a browser or copy this link to open it elsewhere.",
            "Instale um navegador ou copie este link para abri-lo em outro lugar.");
        put("Copy link", "Copiar link");
        put("Link copied.", "Link copiado.");
        put("PHONE-ONLY INSTALLER", "INSTALADOR SEM COMPUTADOR");
        put("No computer required. Select an Xbox 360 ISO that you dumped from your own Skate 3 copy. The game stays on this device.",
            "Nenhum computador é necessário. Selecione uma ISO de Xbox 360 extraída da sua própria cópia de Skate 3. O jogo permanece neste dispositivo.");
        put("Checking installation...", "Verificando a instalação...");
        put("CHECK FOR APP UPDATES", "PROCURAR ATUALIZAÇÕES");
        put("CHECKING FOR UPDATES...", "PROCURANDO ATUALIZAÇÕES...");
        put("APP UP TO DATE", "APLICATIVO ATUALIZADO");
        put("BUG REPORT / DEVICE DIAGNOSTICS", "RELATAR BUG / DIAGNÓSTICO DO DISPOSITIVO");
        put("Requires Android 13+, ARM64, Vulkan, and about 8 GB free after the ISO is already on your device or USB drive. Touch controls are included.",
            "Requer Android 13+, ARM64, Vulkan e cerca de 8 GB livres além da ISO no dispositivo ou unidade USB. Controles de toque estão incluídos.");
        put("DEVICE NOT SUPPORTED", "DISPOSITIVO NÃO COMPATÍVEL");
        put("This build requires a 64-bit ARM Android device.", "Esta versão requer um dispositivo Android ARM de 64 bits.");
        put("This build requires Android 13 or newer.", "Esta versão requer Android 13 ou mais recente.");
        put("This device does not report the required Vulkan support.", "Este dispositivo não informa o suporte Vulkan necessário.");
        put("CLOSE", "FECHAR");
        put("READY TO SKATE", "PRONTO PARA ANDAR DE SKATE");
        put("PLAY SKATE 3", "JOGAR SKATE 3");
        put("REPAIR OR REINSTALL", "REPARAR OU REINSTALAR");
        put("VIEW SETUP LOG", "VER REGISTRO DA INSTALAÇÃO");
        put("VIEW LAST SETUP LOG", "VER ÚLTIMO REGISTRO");
        put("GAME EXTRACTED", "JOGO EXTRAÍDO");
        put("Finish by downloading the verified 1.7 MB Title Update 3, or select the package yourself.",
            "Finalize baixando a Title Update 3 verificada de 1,7 MB ou selecione o pacote manualmente.");
        put("FINISH SETUP AUTOMATICALLY", "FINALIZAR INSTALAÇÃO AUTOMATICAMENTE");
        put("SELECT TITLE UPDATE FILE", "SELECIONAR ARQUIVO DA TITLE UPDATE");
        put("START OVER", "RECOMEÇAR");
        put("ONE FILE NEEDED", "FALTA UM ARQUIVO");
        put("SELECT MY SKATE 3 ISO", "SELECIONAR MINHA ISO DE SKATE 3");
        put("INSPECTING ISO", "ANALISANDO A ISO");
        put("EXTRACTING GAME", "EXTRAINDO O JOGO");
        put("INSTALLING TITLE UPDATE 3", "INSTALANDO TITLE UPDATE 3");
        put("VERIFYING TITLE UPDATE 3", "VERIFICANDO TITLE UPDATE 3");
        put("Downloading and verifying 1.7 MB...", "Baixando e verificando 1,7 MB...");
        put("The Title Update server is temporarily unavailable. The app tried 3 times. Try again later, or use Select Title Update File.",
            "O servidor da Title Update está temporariamente indisponível. O aplicativo tentou 3 vezes. Tente novamente mais tarde ou use Selecionar Arquivo da Title Update.");
        put("Title Update download was interrupted.", "O download da Title Update foi interrompido.");
        put("INSTALLATION COMPLETE", "INSTALAÇÃO CONCLUÍDA");
        put("Your files were verified and stayed on this device.", "Seus arquivos foram verificados e permaneceram neste dispositivo.");
        put("Game installation needs repair.", "A instalação do jogo precisa ser reparada.");
        put("You already have the newest build.", "Você já possui a versão mais recente.");
        put("DOWNLOADING APP UPDATE", "BAIXANDO ATUALIZAÇÃO DO APLICATIVO");
        put("UPDATE VERIFIED", "ATUALIZAÇÃO VERIFICADA");
        put("Android will now open the installer. Your game files stay in place.",
            "O Android abrirá o instalador. Os arquivos do jogo permanecerão no lugar.");
        put("Allow installs from Skate 3, then return here.", "Permita instalações pelo Skate 3 e depois volte aqui.");
        put("Later", "Depois");
        put("Download update", "Baixar atualização");
        put("The app will verify the download, then Android will ask you to tap Install. Your game files and settings stay in place.",
            "O aplicativo verificará o download e o Android pedirá que você toque em Instalar. Os arquivos e configurações do jogo permanecerão no lugar.");
        put("CHARACTERS  •  SEIYU INCLUDED", "PERSONAGENS  •  SEIYU INCLUÍDO");
        put("CHARACTERS  •  RESTORE SEIYU", "PERSONAGENS  •  RESTAURAR SEIYU");
        put("PREPARING SEIYU", "PREPARANDO SEIYU");
        put("Installing the included playable penguin...", "Instalando o pinguim jogável incluído...");
        put("ADVANCED OPTIONS", "OPÇÕES AVANÇADAS");
        put("ADVANCED OPTIONS  •  CUSTOM GPU DRIVER ACTIVE", "OPÇÕES AVANÇADAS  •  DRIVER DE GPU PERSONALIZADO ATIVO");
        put("CLEANING UP", "LIMPANDO");
        put("Removing this app's installed copy...", "Removendo a cópia instalada por este aplicativo...");
        put("Setup needs attention", "A instalação precisa de atenção");
        put("Your original ISO was not changed.", "Sua ISO original não foi alterada.");
        put("Repair or reinstall?", "Reparar ou reinstalar?");
        put("This removes the extracted game files from this app, then lets you select your ISO again. Your original ISO is not changed.",
            "Isso remove os arquivos extraídos pelo aplicativo e permite selecionar a ISO novamente. Sua ISO original não será alterada.");
        put("Discard partial setup?", "Descartar instalação parcial?");
        put("Only the incomplete copy created by this installer will be removed. Your original ISO is not changed.",
            "Somente a cópia incompleta criada pelo instalador será removida. Sua ISO original não será alterada.");
        put("Cancel", "Cancelar");
        put("Continue", "Continuar");
        put("Start over", "Recomeçar");
        put("Setup log", "Registro da instalação");
        put("No setup log has been written yet.", "Nenhum registro de instalação foi criado ainda.");
        put("Launcher language", "Idioma do launcher");
        put("Language changed. The launcher will restart.", "Idioma alterado. O launcher será reiniciado.");
        put("selected file", "arquivo selecionado");
        put("Open Buku313 on GitHub", "Abrir Buku313 no GitHub");
        put("Report a developer-build bug", "Relatar um bug da versão de desenvolvimento");
        put("GitHub will open with this device's safe technical details already filled in. The same details will be copied so you can paste them if the browser removes a field.\n\nNo ISO, game file, save, account name, or private path is included.",
            "O GitHub abrirá com os detalhes técnicos seguros deste dispositivo já preenchidos. Os mesmos detalhes serão copiados para você colar caso o navegador remova algum campo.\n\nNenhuma ISO, arquivo do jogo, save, nome de conta ou caminho privado será incluído.");
        put("Copy only", "Somente copiar");
        put("Open GitHub", "Abrir GitHub");
        put("Device diagnostics copied.", "Diagnóstico do dispositivo copiado.");
        put("No browser is available. The diagnostics are copied.", "Nenhum navegador está disponível. O diagnóstico foi copiado.");
        put("Advanced options", "Opções avançadas");
        put("Experimental GPU driver", "Driver de GPU experimental");
        put("GPU driver experiments", "Experimentos de driver de GPU");
        put("Use System Driver", "Usar Driver do Sistema");
        put("Enable experimental driver?", "Ativar driver experimental?");
        put("Enable anyway", "Ativar mesmo assim");
        put("Remove custom driver?", "Remover driver personalizado?");
        put("Remove", "Remover");
        put("This device does not appear to use a Snapdragon / Adreno GPU. Turnip is not compatible here, so Skate 3 will stay on the System Driver.",
            "Este dispositivo não parece usar uma GPU Snapdragon / Adreno. O Turnip não é compatível aqui, então o Skate 3 continuará no Driver do Sistema.");
        put("System Driver selected for the next launch.", "Driver do Sistema selecionado para a próxima inicialização.");
        put("Custom driver selected for the next launch.", "Driver personalizado selecionado para a próxima inicialização.");
        put("Custom driver removed.", "Driver personalizado removido.");
        put("Update needs attention", "A atualização precisa de atenção");
        put("Mod Store unavailable", "Loja de Mods indisponível");
        put("Mod Store needs attention", "A Loja de Mods precisa de atenção");
        put("MOD STORE", "LOJA DE MODS");
        put("Open Mod Store", "Abrir Loja de Mods");
        put("Close", "Fechar");
        put("Restore bundled copy", "Restaurar cópia incluída");
        put("This removes only the downloaded mod files. Your game installation and original skater are not changed.",
            "Isto remove apenas os arquivos de mod baixados. A instalação do jogo e o skatista original não são alterados.");
        put("Seiyu needs attention", "O Seiyu precisa de atenção");
        put("Restore Seiyu", "Restaurar Seiyu");
        put("Restore Seiyu Paradise Penguin", "Restaurar Seiyu Paradise Penguin");
        put("Seiyu restored and ready.", "Seiyu restaurado e pronto.");
        put("Seiyu installed. Choose it in RB + Start > Mods.", "Seiyu instalado. Escolha-o em RB + Start > Mods.");
        put("Seiyu is bundled with the app and the original skater remains the default. Manual import is available only for development or restoring custom files. In game, press RB + Start and open Mods to switch characters.",
            "O Seiyu vem incluído no aplicativo e o skatista original continua sendo o padrão. A importação manual está disponível apenas para desenvolvimento ou para restaurar arquivos personalizados. No jogo, pressione RB + Start e abra Mods para trocar de personagem.");
        put("Choose files", "Escolher arquivos");
        put("Do not enable this if Skate 3 already works.", "Não ative isto se o Skate 3 já funciona.");
        put("will replace the working System Driver only inside Skate 3. It may crash, black-screen, or render incorrectly. You can return here and restore System Driver.",
            "substituirá o Driver do Sistema funcional apenas dentro do Skate 3. Pode travar, ficar com a tela preta ou renderizar incorretamente. Você pode voltar aqui e restaurar o Driver do Sistema.");
        put("This removes", "Isto remove");
        put("from Skate 3 and selects the System Driver.", "do Skate 3 e seleciona o Driver do Sistema.");
        put("imported but not enabled. System Driver remains selected.",
            "importado, mas não ativado. O Driver do Sistema permanece selecionado.");
        put("installed. Select it in RB + Start > Mods.", "instalado. Selecione-o em RB + Start > Mods.");
        put("removed.", "removido.");
        put("Your existing game and mod files were not changed.",
            "Seus arquivos de jogo e de mods existentes não foram alterados.");
    }

    private LauncherStrings() {}

    static boolean isPortuguese(Context context) {
        return PORTUGUESE_BRAZIL.equals(language(context));
    }

    static String language(Context context) {
        String saved = context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE)
            .getString(LANGUAGE, null);
        if (saved != null) return saved;
        return "pt".equalsIgnoreCase(Locale.getDefault().getLanguage())
            ? PORTUGUESE_BRAZIL : ENGLISH;
    }

    static void setLanguage(Context context, String language) {
        String selected = PORTUGUESE_BRAZIL.equals(language)
            ? PORTUGUESE_BRAZIL : ENGLISH;
        context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE)
            .edit().putString(LANGUAGE, selected).apply();
    }

    static String languageButton(Context context) {
        return isPortuguese(context) ? "🇺🇸 English" : "🇧🇷 Português";
    }

    static String text(Context context, String english) {
        if (!isPortuguese(context) || english == null || english.isEmpty()) return english;
        String exact = PT.get(english);
        if (exact != null) return exact;

        if (english.startsWith("UPDATE TO ")) {
            return "ATUALIZAR PARA " + english.substring("UPDATE TO ".length());
        }
        if (english.startsWith("Game files: ")) {
            return "Arquivos do jogo: " + english.substring("Game files: ".length());
        }
        if (english.contains("\nGame files: ")) {
            return english.replace("\nGame files: ", "\nArquivos do jogo: ");
        }
        if (english.startsWith("Select your Skate 3 Xbox 360 ISO.")) {
            return english.replace(
                "Select your Skate 3 Xbox 360 ISO. It can be in Downloads, on an SD card, or on a connected USB drive.",
                "Selecione sua ISO de Skate 3 para Xbox 360. Ela pode estar em Downloads, em um cartão SD ou em uma unidade USB conectada.")
                .replace("Free space: ", "Espaço livre: ");
        }
        if (english.startsWith("Checking ")) return "Verificando " + english.substring(9);
        if (english.startsWith("Reading ")) return "Lendo " + english.substring(8);
        if (english.startsWith("Starting ")) return "Iniciando " + english.substring(9);
        if (english.startsWith("Downloading Skate 3 ")) {
            return "Baixando Skate 3 " + english.substring("Downloading Skate 3 ".length());
        }
        if (english.startsWith("Retrying Title Update download (")) {
            return english.replace("Retrying Title Update download (",
                "Tentando baixar a Title Update novamente (")
                .replace(" of ", " de ");
        }
        if (english.startsWith("Title Update setup stopped: ")) {
            return "A instalação da Title Update foi interrompida: " +
                text(context, english.substring("Title Update setup stopped: ".length()));
        }
        if (english.startsWith("Setup stopped: ")) {
            return "A instalação foi interrompida: " +
                text(context, english.substring("Setup stopped: ".length()));
        }
        if (english.contains(" of ")) return english.replace(" of ", " de ");
        return english;
    }

    private static void put(String english, String portuguese) {
        PT.put(english, portuguese);
    }
}
